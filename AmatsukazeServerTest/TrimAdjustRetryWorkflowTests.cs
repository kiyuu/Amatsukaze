using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using Amatsukaze.Shared;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class TrimAdjustRetryWorkflowTests
{
    private static readonly DateTime PreviousStart = new(2026, 10, 28, 12, 30, 0, DateTimeKind.Utc);
    private static readonly DateTime RetryStart = PreviousStart.AddMinutes(7);
    private static readonly JsonSerializerOptions QueueJsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        Converters = { new JsonStringEnumConverter() },
    };

    [Theory]
    [InlineData(ProcMode.CMCheck)]
    [InlineData(ProcMode.TrimAdjust)]
    public async Task QueueからEncodingを経て実行開始時刻が更新されたら同じIDのセッションを一度だけ作成する(ProcMode mode)
    {
        var retryMode = mode == ProcMode.CMCheck ? ProcMode.TrimAdjust : mode;
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart, mode) },
            new[] { Item("Queue", null, retryMode) },
            new[] { Item("Encoding", null, retryMode) },
            new[] { Item("Complete", RetryStart, retryMode) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 2,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.True(result.CanCreateSession);
        Assert.Equal("restored-session", result.Session?.SessionId);
        var change = Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
        using (var body = JsonDocument.Parse(change.Body))
        {
            var root = body.RootElement;
            Assert.Equal(42, GetProperty(root, "itemId").GetInt32());
            Assert.Equal("ResetState", GetProperty(root, "changeType").GetString());
            Assert.Equal(PreviousStart, GetProperty(root, "expectedEncodeStart").GetDateTime());
            if (mode == ProcMode.CMCheck)
            {
                Assert.Equal("TrimAdjust", GetProperty(root, "mode").GetString());
            }
            else
            {
                Assert.False(TryGetProperty(root, "mode", out _));
            }
        }
        var session = Assert.Single(handler.Requests, request => request.Path == "/api/trim/sessions");
        using (var body = JsonDocument.Parse(session.Body))
        {
            Assert.Equal(42, GetProperty(body.RootElement, "queueItemId").GetInt32());
            Assert.Equal(2, GetProperty(body.RootElement, "scaleMode").GetInt32());
        }
        Assert.Single(handler.Requests, request => request.Method == HttpMethod.Post && request.Path == "/api/queue/change");
        Assert.Single(handler.Requests, request => request.Method == HttpMethod.Post && request.Path == "/api/trim/sessions");
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/requeue");
    }

    [Theory]
    [InlineData(ProcMode.CMCheck)]
    [InlineData(ProcMode.TrimAdjust)]
    public async Task 強制復元は新しい強制開始操作を一度だけ送信する(ProcMode mode)
    {
        var retryMode = mode == ProcMode.CMCheck ? ProcMode.TrimAdjust : mode;
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart, mode) },
            new[] { Item("Encoding", null, retryMode) },
            new[] { Item("Complete", RetryStart, retryMode) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 2,
            TrimAdjustRetryMode.ForceStart, TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.True(result.CanCreateSession);
        var change = Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
        using (var body = JsonDocument.Parse(change.Body))
        {
            Assert.Equal("ResetStateAndForceStart", GetProperty(body.RootElement, "changeType").GetString());
            Assert.Equal(PreviousStart, GetProperty(body.RootElement, "expectedEncodeStart").GetDateTime());
            if (mode == ProcMode.CMCheck)
            {
                Assert.Equal("TrimAdjust", GetProperty(body.RootElement, "mode").GetString());
            }
            else
            {
                Assert.False(TryGetProperty(body.RootElement, "mode", out _));
            }
        }
        Assert.Single(handler.Requests, request => request.Path == "/api/trim/sessions");
        Assert.Single(handler.Requests, request => request.Method == HttpMethod.Post && request.Path == "/api/queue/change");
    }

    [Theory]
    [InlineData("Queue")]
    [InlineData("Encoding")]
    public async Task 強制復元でも既に実行中の試行は強制再送せず待つ(string activeState)
    {
        var handler = new ScriptedHandler(
            new[] { Item(activeState, null, ProcMode.TrimAdjust) },
            new[] { Item("Complete", RetryStart, ProcMode.TrimAdjust) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TrimAdjustRetryMode.ForceStart, TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.True(result.CanCreateSession);
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.Single(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Fact]
    public async Task 強制復元のHTTP失敗は表示して強制操作を再送しない()
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("Complete", PreviousStart) })
        {
            ChangeStatus = HttpStatusCode.ServiceUnavailable,
            ChangeResponse = "queue endpoint unavailable",
        };
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RetryAndWaitAsync(api, 42, PreviousStart,
            TrimAdjustRetryMode.ForceStart, TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.ApiError, result.Status);
        Assert.Contains("queue endpoint unavailable", result.Message);
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Theory]
    [InlineData("Queue")]
    [InlineData("Encoding")]
    public async Task 既に別画面でリトライ中なら監視して新しい完了を待ち再送しない(string activeState)
    {
        var handler = new ScriptedHandler(
            new[] { Item(activeState, null, ProcMode.TrimAdjust) },
            new[] { Item("Complete", RetryStart, ProcMode.TrimAdjust) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.True(result.CanCreateSession);
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.Single(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Theory]
    [InlineData("Queue")]
    [InlineData("Encoding")]
    public async Task 初期CMCheckが処理中のままならセッション作成前に終了する(string state)
    {
        var handler = new ScriptedHandler(
            new[] { Item(state, null, ProcMode.CMCheck) },
            new[] { Item(state, null, ProcMode.CMCheck) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.InvalidState, result.Status);
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Fact]
    public async Task CMCheckの再試行応答がTrimAdjustへ変わっていなければセッションを作成しない()
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart, ProcMode.CMCheck) },
            new[] { Item("Complete", RetryStart, ProcMode.CMCheck) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.InvalidState, result.Status);
        var change = Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
        using var body = JsonDocument.Parse(change.Body);
        Assert.Equal("TrimAdjust", GetProperty(body.RootElement, "mode").GetString());
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Fact]
    public async Task 初期LogoPendingは保留理由を表示して終了する()
    {
        var handler = new ScriptedHandler(new[] { Item("LogoPending", null, stateDetailText: " / CM解析の並列数上限") });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.Pending, result.Status);
        Assert.Contains("保留理由: CM解析の並列数上限", result.Message);
        Assert.DoesNotContain("保留理由: /", result.Message);
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Fact]
    public async Task リトライ後の最初の確認で新しい完了時刻ならすぐセッションを作成する()
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("Complete", RetryStart, ProcMode.TrimAdjust) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.True(result.CanCreateSession);
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.Single(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Fact]
    public async Task HTTP200のエラー通知は拒否として扱い古い完了状態のまま再送しない()
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("Complete", PreviousStart) })
        {
            ChangeResponse = "{\"ok\":true,\"level\":\"error\",\"message\":\"状態が変わっています\"}",
        };
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.Rejected, result.Status);
        Assert.Contains("状態が変わっています", result.Message);
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Fact]
    public async Task HTTP失敗は元の理由を表示して再送しない()
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("Complete", PreviousStart) })
        {
            ChangeStatus = HttpStatusCode.ServiceUnavailable,
            ChangeResponse = "queue endpoint unavailable",
        };
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);
        using var cancellation = new CancellationTokenSource();
        var waitCalls = 0;

        var result = await TrimAdjustRetryWorkflow.RetryAndWaitAsync(api, 42, PreviousStart,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), cancellation.Token,
            (_, _) =>
            {
                waitCalls++;
                cancellation.Cancel();
                return Task.CompletedTask;
            });

        Assert.Equal(TrimAdjustRetryStatus.ApiError, result.Status);
        Assert.Contains("queue endpoint unavailable", result.Message);
        Assert.Equal(0, waitCalls);
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
    }

    [Fact]
    public async Task 送信例外は元の理由を表示して再送しない()
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("Complete", PreviousStart) })
        {
            ChangeException = new HttpRequestException("connection reset"),
        };
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);
        using var cancellation = new CancellationTokenSource();
        var waitCalls = 0;

        var result = await TrimAdjustRetryWorkflow.RetryAndWaitAsync(api, 42, PreviousStart,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), cancellation.Token,
            (_, _) =>
            {
                waitCalls++;
                cancellation.Cancel();
                return Task.CompletedTask;
            });

        Assert.Equal(TrimAdjustRetryStatus.ApiError, result.Status);
        Assert.Contains("connection reset", result.Message);
        Assert.Equal(0, waitCalls);
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
    }

    [Fact]
    public async Task 初回キュー取得が停止されたらResetを送信しない()
    {
        using var cancellation = new CancellationTokenSource();
        var handler = new BlockingQueueHandler();
        using var http = new HttpClient(handler) { BaseAddress = new Uri("http://localhost") };
        var api = new AmatsukazeApi(http);

        var pending = TrimAdjustRetryWorkflow.RetryAndWaitAsync(api, 42, PreviousStart, cancellation.Token);
        await handler.RequestStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        cancellation.Cancel();
        var result = await pending;

        Assert.Equal(TrimAdjustRetryStatus.Canceled, result.Status);
        Assert.Equal(1, handler.QueueRequests);
    }

    [Fact]
    public async Task 初期Queueは開始期限でタイムアウトする()
    {
        var handler = new ScriptedHandler(new[] { Item("Queue", null, ProcMode.TrimAdjust) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RetryAndWaitAsync(api, 42, PreviousStart,
            TimeSpan.Zero, TimeSpan.Zero, CancellationToken.None, CompleteWait);

        Assert.Equal(TimeSpan.FromSeconds(30), TrimAdjustRetryWorkflow.AcceptanceConfirmationTimeout);
        Assert.Equal(TrimAdjustRetryStatus.TimedOut, result.Status);
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/queue/change");
    }

    [Fact]
    public async Task リセット後もQueueならタイムアウトしリセットを再送しない()
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("Queue", null, ProcMode.TrimAdjust) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RetryAndWaitAsync(api, 42, PreviousStart,
            TimeSpan.Zero, TimeSpan.Zero, CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.TimedOut, result.Status);
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Fact]
    public async Task リセット後のLogoPendingは保留理由を表示して終了する()
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("LogoPending", null, ProcMode.TrimAdjust, stateDetailText: " / CM解析の並列数上限") });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.Pending, result.Status);
        Assert.Contains("保留理由: CM解析の並列数上限", result.Message);
        Assert.DoesNotContain("保留理由: /", result.Message);
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Fact]
    public async Task 新しい完了を取得中に停止されたらセッション作成へ進まない()
    {
        using var cancellation = new CancellationTokenSource();
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("Complete", RetryStart, ProcMode.TrimAdjust) })
        {
            OnQueueRequest = requestNumber =>
            {
                if (requestNumber == 2) cancellation.Cancel();
            },
        };
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), cancellation.Token, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.Canceled, result.Status);
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Fact]
    public async Task セッションPOST中に停止されたら成功結果を返さない()
    {
        using var cancellation = new CancellationTokenSource();
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("Complete", RetryStart, ProcMode.TrimAdjust) })
        {
            OnSessionRequest = cancellation.Cancel,
        };
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), cancellation.Token, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.Canceled, result.Status);
        Assert.Null(result.Session);
        Assert.Single(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Fact]
    public async Task 古い完了状態のままなら受付確認期限で止め再送しない()
    {
        var handler = new ScriptedHandler(new[] { Item("Complete", PreviousStart) })
        {
            ChangeResponse = "{\"ok\":true,\"level\":\"info\",\"message\":\"受け付けました\"}",
        };
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RetryAndWaitAsync(api, 42, PreviousStart,
            TimeSpan.FromMilliseconds(1), TimeSpan.FromMilliseconds(100), CancellationToken.None,
            (delay, token) => Task.Delay(delay, token));

        Assert.Equal(TrimAdjustRetryStatus.TimedOut, result.Status);
        Assert.Contains("再送していません", result.Message);
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
    }

    [Fact]
    public async Task 待機停止は監視だけを止めキュー取消APIを呼ばない()
    {
        using var cancellation = new CancellationTokenSource();
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("Encoding", null, ProcMode.TrimAdjust) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RetryAndWaitAsync(api, 42, PreviousStart,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), cancellation.Token,
            (_, _) =>
            {
                cancellation.Cancel();
                return Task.CompletedTask;
            });

        Assert.Equal(TrimAdjustRetryStatus.Canceled, result.Status);
        Assert.Contains("キューの処理は継続", result.Message);
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.DoesNotContain(handler.Requests, request => request.Path.Contains("cancel", StringComparison.OrdinalIgnoreCase));
    }

    [Theory]
    [InlineData("Failed", TrimAdjustRetryStatus.Failed)]
    [InlineData("Canceled", TrimAdjustRetryStatus.Canceled)]
    public async Task 新しい試行の失敗または取消で監視を終了する(string state, TrimAdjustRetryStatus expectedStatus)
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("Encoding", null, ProcMode.TrimAdjust) },
            new[] { Item(state, null, ProcMode.TrimAdjust) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(expectedStatus, result.Status);
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
    }

    [Fact]
    public async Task 待機中に項目が削除されたら終了する()
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            Array.Empty<QueueItemView>());
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.QueueItemRemoved, result.Status);
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Theory]
    [InlineData(ProcMode.Batch)]
    [InlineData(ProcMode.AutoBatch)]
    public async Task 完了済みバッチはカット調整へ切り替えて同じ項目を再試行する(ProcMode mode)
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart, mode) },
            new[] { Item("Encoding", null, ProcMode.TrimAdjust) },
            new[] { Item("Complete", RetryStart, ProcMode.TrimAdjust) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TrimAdjustRetryMode.SwitchBatchToTrimAdjust, TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.True(result.CanCreateSession);
        var change = Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
        using (var body = JsonDocument.Parse(change.Body))
        {
            var root = body.RootElement;
            Assert.Equal(42, GetProperty(root, "itemId").GetInt32());
            Assert.Equal("ResetState", GetProperty(root, "changeType").GetString());
            Assert.Equal(PreviousStart, GetProperty(root, "expectedEncodeStart").GetDateTime());
            Assert.Equal("TrimAdjust", GetProperty(root, "mode").GetString());
        }
        Assert.Single(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Theory]
    [InlineData(ProcMode.Batch, TrimAdjustRetryMode.Normal)]
    [InlineData(ProcMode.Batch, TrimAdjustRetryMode.ForceStart)]
    [InlineData(ProcMode.AutoBatch, TrimAdjustRetryMode.Normal)]
    [InlineData(ProcMode.AutoBatch, TrimAdjustRetryMode.ForceStart)]
    public async Task バッチは明示確認モード以外ではPOSTしない(ProcMode mode, TrimAdjustRetryMode retryMode)
    {
        var handler = new ScriptedHandler(new[] { Item("Complete", PreviousStart, mode) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            retryMode, TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.BatchMode, result.Status);
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Theory]
    [InlineData(ProcMode.Batch)]
    [InlineData(ProcMode.AutoBatch)]
    public async Task 実行中のバッチは変更せず再試行要求を送らない(ProcMode mode)
    {
        var handler = new ScriptedHandler(new[] { Item("Encoding", null, mode) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.BatchMode, result.Status);
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Theory]
    [InlineData(ProcMode.Batch)]
    [InlineData(ProcMode.AutoBatch)]
    public async Task 完了時刻が変わったバッチは再投入もセッション作成もしない(ProcMode mode)
    {
        var handler = new ScriptedHandler(new[] { Item("Complete", RetryStart, mode) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TrimAdjustRetryMode.SwitchBatchToTrimAdjust, TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.InvalidState, result.Status);
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Fact]
    public async Task Encoding後にQueueへ戻ったらセッション作成へ進まない()
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("Encoding", null, ProcMode.TrimAdjust) },
            new[] { Item("Queue", null, ProcMode.TrimAdjust) });
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.InvalidState, result.Status);
        Assert.Contains("Encoding開始後", result.Message);
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
        Assert.DoesNotContain(handler.Requests, request => request.Path == "/api/trim/sessions");
    }

    [Fact]
    public async Task 解析完了後もキャッシュが無ければセッション作成で終了し再確認しない()
    {
        var handler = new ScriptedHandler(
            new[] { Item("Complete", PreviousStart) },
            new[] { Item("Complete", RetryStart, ProcMode.TrimAdjust) })
        {
            SessionStatus = HttpStatusCode.BadRequest,
            SessionResponse = "{\"message\":\"amts0.datが見つかりません\",\"errorCode\":\"missing_source_cache\"}",
        };
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await TrimAdjustRetryWorkflow.RecoverSessionAsync(api, 42, PreviousStart, 1,
            TimeSpan.Zero, TimeSpan.FromMinutes(1), CancellationToken.None, CompleteWait);

        Assert.Equal(TrimAdjustRetryStatus.SessionCreationFailed, result.Status);
        Assert.Contains("カット調整モードの処理は完了しました", result.Message);
        Assert.Single(handler.Requests, request => request.Path == "/api/trim/sessions");
        Assert.Single(handler.Requests, request => request.Path == "/api/queue/change");
    }

    [Fact]
    public async Task セッションAPIのmissing_source_cacheコードを保持する()
    {
        var handler = new ScriptedHandler
        {
            SessionStatus = HttpStatusCode.BadRequest,
            SessionResponse = "{\"message\":\"amts0.datが見つかりません\",\"errorCode\":\"missing_source_cache\"}",
        };
        using var http = CreateHttpClient(handler);
        var api = new AmatsukazeApi(http);

        var result = await api.CreateTrimSessionAsync(new TrimAdjustSessionRequest { QueueItemId = 42 });

        Assert.False(result.Ok);
        Assert.Equal("missing_source_cache", result.ErrorCode);
        Assert.Contains("amts0.dat", result.Error);
    }

    private static QueueItemView Item(
        string state,
        DateTime? encodeStart,
        ProcMode mode = ProcMode.CMCheck,
        string? stateDetailText = null) => new()
    {
        Id = 42,
        Mode = mode,
        State = state,
        EncodeStart = encodeStart,
        StateDetailText = stateDetailText,
    };

    private static HttpClient CreateHttpClient(ScriptedHandler handler) =>
        new(handler) { BaseAddress = new Uri("http://localhost") };

    private static Task CompleteWait(TimeSpan _, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        return Task.CompletedTask;
    }

    private static JsonElement GetProperty(JsonElement root, string name)
    {
        if (TryGetProperty(root, name, out var value)) return value;
        throw new Xunit.Sdk.XunitException($"JSON property not found: {name}");
    }

    private static bool TryGetProperty(JsonElement root, string name, out JsonElement value)
    {
        foreach (var property in root.EnumerateObject())
        {
            if (string.Equals(property.Name, name, StringComparison.OrdinalIgnoreCase))
            {
                value = property.Value;
                return true;
            }
        }
        value = default;
        return false;
    }

    private sealed record CapturedRequest(HttpMethod Method, string Path, string Body);

    private sealed class BlockingQueueHandler : HttpMessageHandler
    {
        public TaskCompletionSource RequestStarted { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public int QueueRequests { get; private set; }

        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            if (request.RequestUri?.AbsolutePath == "/api/queue")
            {
                QueueRequests++;
                RequestStarted.TrySetResult();
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            }
            throw new InvalidOperationException($"Unexpected HTTP request: {request.Method} {request.RequestUri?.AbsolutePath}");
        }
    }

    private sealed class ScriptedHandler : HttpMessageHandler
    {
        private readonly Queue<QueueItemView[]> queueSnapshots = new();
        private QueueItemView[]? lastQueueSnapshot;

        public List<CapturedRequest> Requests { get; } = new();
        public string ChangeResponse { get; set; } = "{\"ok\":true,\"level\":\"info\"}";
        public HttpStatusCode ChangeStatus { get; set; } = HttpStatusCode.OK;
        public Exception? ChangeException { get; set; }
        public string SessionResponse { get; set; } = "{\"sessionId\":\"restored-session\"}";
        public HttpStatusCode SessionStatus { get; set; } = HttpStatusCode.OK;
        public Action<int>? OnQueueRequest { get; set; }
        public Action? OnSessionRequest { get; set; }
        private int queueRequestCount;

        public ScriptedHandler(params QueueItemView[][] snapshots)
        {
            foreach (var snapshot in snapshots) queueSnapshots.Enqueue(snapshot);
        }

        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            var path = request.RequestUri?.AbsolutePath ?? "";
            var body = request.Content == null ? "" : await request.Content.ReadAsStringAsync(cancellationToken);
            Requests.Add(new CapturedRequest(request.Method, path, body));

            if (request.Method == HttpMethod.Get && path == "/api/queue")
            {
                OnQueueRequest?.Invoke(++queueRequestCount);
                if (queueSnapshots.Count > 0) lastQueueSnapshot = queueSnapshots.Dequeue();
                if (lastQueueSnapshot == null) throw new InvalidOperationException("Queue snapshot was not scripted");
                var view = new QueueView { Items = lastQueueSnapshot.ToList() };
                return JsonResponse(HttpStatusCode.OK, JsonSerializer.Serialize(view, QueueJsonOptions));
            }
            if (request.Method == HttpMethod.Post && path == "/api/queue/change")
            {
                if (ChangeException != null) throw ChangeException;
                return JsonResponse(ChangeStatus, ChangeResponse);
            }
            if (request.Method == HttpMethod.Post && path == "/api/trim/sessions")
            {
                OnSessionRequest?.Invoke();
                return JsonResponse(SessionStatus, SessionResponse);
            }
            throw new InvalidOperationException($"Unexpected HTTP request: {request.Method} {path}");
        }

        private static HttpResponseMessage JsonResponse(HttpStatusCode status, string body) => new(status)
        {
            Content = new StringContent(body, Encoding.UTF8, "application/json"),
        };
    }
}
