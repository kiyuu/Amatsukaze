using System;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Shared
{
    public enum TrimAdjustRetryMode
    {
        Normal,
        ForceStart
    }

    public enum TrimAdjustRetryStatus
    {
        Ready,
        QueueItemRemoved,
        BatchMode,
        UnsupportedMode,
        MissingExpectedEncodeStart,
        Failed,
        Canceled,
        SessionCreationFailed,
        InvalidState,
        ApiError,
        Rejected,
        TimedOut
    }

    public sealed class TrimAdjustRetryResult
    {
        public TrimAdjustRetryStatus Status { get; init; }
        public string? Message { get; init; }
        public TrimAdjustSessionResponse? Session { get; init; }
        public bool CanCreateSession => Status == TrimAdjustRetryStatus.Ready;
    }

    public static class TrimAdjustRetryWorkflow
    {
        public static readonly TimeSpan QueuePollInterval = TimeSpan.FromSeconds(2);
        public static readonly TimeSpan AcceptanceConfirmationTimeout = TimeSpan.FromSeconds(30);

        public static Task<TrimAdjustRetryResult> RecoverSessionAsync(
            IAmatsukazeApi api,
            int queueItemId,
            DateTime? expectedEncodeStart,
            int scaleMode,
            CancellationToken cancellationToken)
            => RecoverSessionAsync(api, queueItemId, expectedEncodeStart, scaleMode, TrimAdjustRetryMode.Normal,
                QueuePollInterval, AcceptanceConfirmationTimeout, cancellationToken, Task.Delay);

        public static Task<TrimAdjustRetryResult> RecoverSessionAsync(
            IAmatsukazeApi api,
            int queueItemId,
            DateTime? expectedEncodeStart,
            int scaleMode,
            TrimAdjustRetryMode retryMode,
            CancellationToken cancellationToken)
            => RecoverSessionAsync(api, queueItemId, expectedEncodeStart, scaleMode, retryMode,
                QueuePollInterval, AcceptanceConfirmationTimeout, cancellationToken, Task.Delay);

        public static Task<TrimAdjustRetryResult> RecoverSessionAsync(
            IAmatsukazeApi api,
            int queueItemId,
            DateTime? expectedEncodeStart,
            int scaleMode,
            TimeSpan pollInterval,
            TimeSpan confirmationTimeout,
            CancellationToken cancellationToken,
            Func<TimeSpan, CancellationToken, Task> waitAsync)
            => RecoverSessionAsync(api, queueItemId, expectedEncodeStart, scaleMode, TrimAdjustRetryMode.Normal,
                pollInterval, confirmationTimeout, cancellationToken, waitAsync);

        public static async Task<TrimAdjustRetryResult> RecoverSessionAsync(
            IAmatsukazeApi api,
            int queueItemId,
            DateTime? expectedEncodeStart,
            int scaleMode,
            TrimAdjustRetryMode retryMode,
            TimeSpan pollInterval,
            TimeSpan confirmationTimeout,
            CancellationToken cancellationToken,
            Func<TimeSpan, CancellationToken, Task> waitAsync)
        {
            try
            {
                cancellationToken.ThrowIfCancellationRequested();
                var retryResult = await RetryAndWaitAsync(api, queueItemId, expectedEncodeStart, retryMode,
                    pollInterval, confirmationTimeout, cancellationToken, waitAsync);
                cancellationToken.ThrowIfCancellationRequested();
                if (!retryResult.CanCreateSession)
                {
                    return retryResult;
                }

                cancellationToken.ThrowIfCancellationRequested();
                var sessionResult = await api.CreateTrimSessionAsync(new TrimAdjustSessionRequest
                {
                    QueueItemId = queueItemId,
                    ScaleMode = scaleMode
                }, cancellationToken);
                cancellationToken.ThrowIfCancellationRequested();
                if (!sessionResult.Ok || sessionResult.Data == null)
                {
                    var message = sessionResult.ErrorCode == "missing_source_cache"
                        ? $"CM解析は完了しましたが、カット調整データを復元できませんでした。{sessionResult.Error}"
                        : sessionResult.Error ?? "セッションの作成に失敗しました";
                    return Result(TrimAdjustRetryStatus.SessionCreationFailed, message);
                }

                return new TrimAdjustRetryResult
                {
                    Status = TrimAdjustRetryStatus.Ready,
                    Session = sessionResult.Data
                };
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return Result(TrimAdjustRetryStatus.Canceled, "待機を停止しました。キューの処理は継続しています。");
            }
        }

        public static Task<TrimAdjustRetryResult> RetryAndWaitAsync(
            IAmatsukazeApi api,
            int queueItemId,
            DateTime? expectedEncodeStart,
            CancellationToken cancellationToken)
            => RetryAndWaitAsync(api, queueItemId, expectedEncodeStart, TrimAdjustRetryMode.Normal,
                QueuePollInterval, AcceptanceConfirmationTimeout, cancellationToken, Task.Delay);

        public static Task<TrimAdjustRetryResult> RetryAndWaitAsync(
            IAmatsukazeApi api,
            int queueItemId,
            DateTime? expectedEncodeStart,
            TimeSpan pollInterval,
            TimeSpan confirmationTimeout,
            CancellationToken cancellationToken,
            Func<TimeSpan, CancellationToken, Task> waitAsync)
            => RetryAndWaitAsync(api, queueItemId, expectedEncodeStart, TrimAdjustRetryMode.Normal,
                pollInterval, confirmationTimeout, cancellationToken, waitAsync);

        public static async Task<TrimAdjustRetryResult> RetryAndWaitAsync(
            IAmatsukazeApi api,
            int queueItemId,
            DateTime? expectedEncodeStart,
            TrimAdjustRetryMode retryMode,
            TimeSpan pollInterval,
            TimeSpan confirmationTimeout,
            CancellationToken cancellationToken,
            Func<TimeSpan, CancellationToken, Task> waitAsync)
        {
            if (api == null) throw new ArgumentNullException(nameof(api));
            if (waitAsync == null) throw new ArgumentNullException(nameof(waitAsync));
            if (queueItemId <= 0)
            {
                return Result(TrimAdjustRetryStatus.InvalidState, "キューアイテムIDが不正です。");
            }
            if (!expectedEncodeStart.HasValue)
            {
                return Result(TrimAdjustRetryStatus.MissingExpectedEncodeStart,
                    "キューの完了時刻を確認できないため、安全なリトライ条件を作れません。キュー画面で状態を確認してください。");
            }

            try
            {
                cancellationToken.ThrowIfCancellationRequested();
                var initial = await api.GetQueueAsync(null, cancellationToken);
                cancellationToken.ThrowIfCancellationRequested();
                if (!initial.Ok || initial.Data == null)
                {
                    return Result(TrimAdjustRetryStatus.ApiError,
                        initial.Error ?? "キュー状態を取得できませんでした。");
                }

                var item = initial.Data.Items.FirstOrDefault(candidate => candidate.Id == queueItemId);
                if (item == null)
                {
                    return Result(TrimAdjustRetryStatus.QueueItemRemoved, "キューアイテムが削除されています。");
                }

                var modeError = CheckMode(item.Mode);
                if (modeError != null) return modeError;

                var terminal = CheckTerminalState(item.State, item.StateDetailText);
                if (terminal != null) return terminal;

                if (IsComplete(item.State))
                {
                    if (!item.EncodeStart.HasValue)
                    {
                        return Result(TrimAdjustRetryStatus.InvalidState,
                            "キューの完了時刻が空のため、安全なリトライ条件を確認できません。");
                    }
                    if (item.EncodeStart.Value != expectedEncodeStart.Value)
                    {
                        return Result(TrimAdjustRetryStatus.Ready, "別画面でのリトライが完了しています。");
                    }
                }
                else if (!IsActive(item.State))
                {
                    return Result(TrimAdjustRetryStatus.InvalidState,
                        $"キュー状態がリトライ可能な完了または実行中ではありません: {item.State ?? "不明"}");
                }

                ApiResult<bool>? retryResult = null;
                var retrySent = false;
                if (IsComplete(item.State))
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    retrySent = true;
                    retryResult = await api.ChangeQueueAsync(new ChangeItemData
                    {
                        ItemId = queueItemId,
                        ChangeType = retryMode == TrimAdjustRetryMode.ForceStart
                            ? ChangeItemType.ResetStateAndForceStart
                            : ChangeItemType.ResetState,
                        ExpectedEncodeStart = expectedEncodeStart.Value
                    }, cancellationToken);
                    cancellationToken.ThrowIfCancellationRequested();
                }

                var acceptanceTimer = Stopwatch.StartNew();
                var observedNewAttempt = IsActive(item.State);
                while (true)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    if (!observedNewAttempt && acceptanceTimer.Elapsed >= confirmationTimeout)
                    {
                        return Result(TrimAdjustRetryStatus.TimedOut, GetAcceptanceTimeoutMessage(confirmationTimeout));
                    }

                    var latest = await api.GetQueueAsync(null, cancellationToken);
                    cancellationToken.ThrowIfCancellationRequested();
                    if (!latest.Ok || latest.Data == null)
                    {
                        return Result(TrimAdjustRetryStatus.ApiError,
                            latest.Error ?? "キュー状態の確認に失敗しました。送信したリトライは再送していません。");
                    }

                    item = latest.Data.Items.FirstOrDefault(candidate => candidate.Id == queueItemId);
                    if (item == null)
                    {
                        return Result(TrimAdjustRetryStatus.QueueItemRemoved, "待機中にキューアイテムが削除されました。");
                    }

                    modeError = CheckMode(item.Mode);
                    if (modeError != null) return modeError;

                    terminal = CheckTerminalState(item.State, item.StateDetailText);
                    if (terminal != null) return terminal;

                    if (IsComplete(item.State))
                    {
                        if (item.EncodeStart.HasValue && item.EncodeStart.Value != expectedEncodeStart.Value)
                        {
                            cancellationToken.ThrowIfCancellationRequested();
                            return Result(TrimAdjustRetryStatus.Ready, null);
                        }
                        if (retrySent && retryResult is { Ok: false })
                        {
                            var error = retryResult.Error ?? "キュー操作に失敗しました。";
                            return retryResult.StatusCode == 200
                                ? Result(TrimAdjustRetryStatus.Rejected, error)
                                : Result(TrimAdjustRetryStatus.ApiError, $"キューのリトライ要求に失敗しました: {error}");
                        }
                    }
                    else if (IsActive(item.State))
                    {
                        observedNewAttempt = true;
                    }
                    else
                    {
                        return Result(TrimAdjustRetryStatus.InvalidState,
                            $"待機中に予期しないキュー状態になりました: {item.State ?? "不明"}");
                    }

                    await waitAsync(pollInterval, cancellationToken);
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return Result(TrimAdjustRetryStatus.Canceled, "待機を停止しました。キューの処理は継続しています。");
            }
            catch (Exception ex)
            {
                return Result(TrimAdjustRetryStatus.ApiError,
                    $"リトライ後のキュー状態を確認できませんでした。送信したリトライは再送していません。{ex.Message}");
            }
        }

        private static TrimAdjustRetryResult? CheckMode(ProcMode mode)
        {
            if (mode == ProcMode.Batch || mode == ProcMode.AutoBatch)
            {
                return Result(TrimAdjustRetryStatus.BatchMode,
                    "バッチ/自動バッチ項目はフルエンコードを実行するため、この画面から自動再試行できません。キュー画面でCM解析モードを選択して再実行してください。");
            }
            if (mode != ProcMode.CMCheck && mode != ProcMode.TrimAdjust)
            {
                return Result(TrimAdjustRetryStatus.UnsupportedMode,
                    "CM解析またはTrim調整の完了項目だけをこの画面から再試行できます。");
            }
            return null;
        }

        private static TrimAdjustRetryResult? CheckTerminalState(string? state, string? detail)
        {
            if (IsCanceled(state))
            {
                return Result(TrimAdjustRetryStatus.Canceled,
                    string.IsNullOrWhiteSpace(detail) ? "キュー項目はキャンセルされています。" : detail);
            }
            if (IsFailed(state))
            {
                return Result(TrimAdjustRetryStatus.Failed,
                    string.IsNullOrWhiteSpace(detail) ? "キュー項目の処理に失敗しました。" : detail);
            }
            return null;
        }

        private static bool IsComplete(string? state) => string.Equals(state, "Complete", StringComparison.OrdinalIgnoreCase);

        private static bool IsActive(string? state) =>
            string.Equals(state, "Queue", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(state, "LogoPending", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(state, "Encoding", StringComparison.OrdinalIgnoreCase);

        private static bool IsFailed(string? state) =>
            string.Equals(state, "Failed", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(state, "PreFailed", StringComparison.OrdinalIgnoreCase);

        private static bool IsCanceled(string? state) => string.Equals(state, "Canceled", StringComparison.OrdinalIgnoreCase);

        private static string GetAcceptanceTimeoutMessage(TimeSpan timeout)
        {
            var timeoutText = timeout.TotalMinutes >= 1
                ? $"{Math.Max(1, (int)Math.Ceiling(timeout.TotalMinutes))}分"
                : $"{Math.Max(1, (int)Math.Ceiling(timeout.TotalSeconds))}秒";
            return $"キューが{timeoutText}以内にリトライを受け付けたことを確認できませんでした。送信したリトライは再送していません。キュー画面で状態を確認してください。";
        }

        private static TrimAdjustRetryResult Result(TrimAdjustRetryStatus status, string? message) =>
            new TrimAdjustRetryResult { Status = status, Message = message };
    }
}
