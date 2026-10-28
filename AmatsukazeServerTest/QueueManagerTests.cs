using Amatsukaze.Server;
using Amatsukaze.Shared;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading.Tasks;
using Xunit;
using ChangeItemData = Amatsukaze.Server.ChangeItemData;
using ChangeItemType = Amatsukaze.Server.ChangeItemType;
using ProcMode = Amatsukaze.Server.ProcMode;
using OutputInfo = Amatsukaze.Server.OutputInfo;
using AddQueueRequest = Amatsukaze.Server.AddQueueRequest;

namespace AmatsukazeServerTest;

public sealed class QueueManagerTests
{
    [Theory]
    [InlineData(new[] { 2, 4 }, 5, new[] { 1, 3, 5, 2, 4 })]
    [InlineData(new[] { 4, 2 }, 0, new[] { 4, 2, 1, 3, 5 })]
    [InlineData(new[] { 2, 4 }, 3, new[] { 1, 3, 2, 4, 5 })]
    [InlineData(new[] { 2, 2, 99 }, -10, new[] { 2, 1, 3, 4, 5 })]
    [InlineData(new[] { 3 }, 100, new[] { 1, 2, 4, 5, 3 })]
    [InlineData(new[] { 5, 4, 3, 2, 1 }, 2, new[] { 5, 4, 3, 2, 1 })]
    public async Task 複数移動は要求順と移動前の挿入位置を守る(int[] ids, int drop, int[] expected)
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(Enumerable.Range(1, 5).Select(i => RestQueueTestFixture.Item(i)).ToArray());
        await fixture.Manager.MoveItems(new QueueMoveManyRequest { ItemIds = ids.ToList(), DropIndex = drop });
        Assert.Equal(expected, fixture.Manager.Queue.Select(i => i.Id));
        Assert.Equal(Enumerable.Range(0, 5), fixture.Manager.Queue.Select(i => i.Order));
        Assert.Equal(expected, fixture.Store.GetQueueView(null!).Items.Select(i => i.Id));
    }

    [Theory]
    [InlineData(null)]
    [InlineData(new int[0])]
    [InlineData(new[] { 99 })]
    public async Task 移動対象が無ければキューと通知版を変更しない(int[]? ids)
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1));
        var version = fixture.Store.GetQueueView(null!).Version;
        await fixture.Manager.MoveItems(ids == null ? null! : new QueueMoveManyRequest { ItemIds = ids.ToList() });
        Assert.Equal(new[] { 1 }, fixture.Manager.Queue.Select(i => i.Id));
        Assert.Equal(version, fixture.Store.GetQueueView(null!).Version);
    }

    [Fact]
    public void スナップショットは無効項目を除外しリスト変更を隔離する()
    {
        var fixture = new RestQueueTestFixture();
        var item = RestQueueTestFixture.Item(1);
        fixture.Manager.Queue.AddRange(new[] { item, null!, new QueueItem { Id = 2, SrcPath = "" } });
        var snapshot = fixture.Manager.GetQueueSnapshot();
        Assert.Same(item, Assert.Single(snapshot));
        snapshot.Clear();
        Assert.Equal(3, fixture.Manager.Queue.Count);
    }

    [Fact]
    public async Task 単一移動と優先度変更を状態ストアへ通知する()
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1), RestQueueTestFixture.Item(2));
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = 1, ChangeType = ChangeItemType.Move, Position = 1 });
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = 1, ChangeType = ChangeItemType.Priority, Priority = 5 });
        Assert.Equal(new[] { 2, 1 }, fixture.Manager.GetQueueSnapshot().Select(i => i.Id));
        Assert.Equal(5, fixture.Store.GetQueueView(null!).Items.Last().Priority);
        Assert.False(fixture.Store.GetLastOperationResult().IsFailed);
    }

    [Theory]
    [InlineData(0, ChangeItemType.ResetState, "指定されたアイテムが見つかりません")]
    [InlineData(1, ChangeItemType.ResetState, "エンコード中のアイテムはリトライできません")]
    [InlineData(1, ChangeItemType.UpdateProfile, "エンコード中のアイテムはプロファイル更新できません")]
    [InlineData(1, ChangeItemType.Profile, "エンコード中はプロファイル変更できません")]
    [InlineData(2, ChangeItemType.ResetState, "このアイテムは追加処理に失敗しているため操作できません")]
    [InlineData(2, ChangeItemType.Profile, "このアイテムはプロファイル変更できません")]
    [InlineData(3, ChangeItemType.Duplicate, "通常モードで追加されたアイテムは複製できません")]
    [InlineData(3, ChangeItemType.ForceStart, "待ち状態にないアイテムは開始できません")]
    public async Task 不正な状態の操作は拒否してキューを維持する(int id, ChangeItemType action, string error)
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1, QueueState.Encoding),
            RestQueueTestFixture.Item(2, QueueState.PreFailed), RestQueueTestFixture.Item(3, QueueState.LogoPending));
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = id, ChangeType = action });
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
        Assert.Contains(error, fixture.Store.GetLastOperationResult().Message);
        Assert.Equal(new[] { QueueState.Encoding, QueueState.PreFailed, QueueState.LogoPending }, fixture.Manager.Queue.Select(i => i.State));
    }

    [Fact]
    public async Task 完了時刻が一致する条件付きリトライは同じCM解析項目をリセットする()
    {
        var fixture = new RestQueueTestFixture();
        var expectedStart = new DateTime(2026, 10, 28, 12, 30, 0, DateTimeKind.Utc);
        var profile = new ProfileSetting { Name = "標準", LastUpdate = DateTime.MinValue };
        var item = RestQueueTestFixture.Item(7, QueueState.Complete);
        item.Mode = ProcMode.CMCheck;
        item.ServiceId = -1;
        item.EncodeStart = expectedStart;
        item.ProfileName = profile.Name;
        item.Profile = profile;
        RestQueueTestFixture.SetField(fixture.Server, "profiles", new Dictionary<string, ProfileSetting> { [profile.Name] = profile });
        RestQueueTestFixture.SetProperty(fixture.Server, "PendingProfile", new ProfileSetting { Name = "未選択" });
        RestQueueTestFixture.SetProperty(fixture.Server, "AppData_", new EncodeServer.AppData
        {
            setting = new Setting(),
            services = new ServiceSetting { ServiceMap = new Dictionary<int, ServiceSettingElement>() },
        });
        await fixture.Seed(item);

        await fixture.Manager.ChangeItem(new ChangeItemData
        {
            ItemId = item.Id,
            ChangeType = ChangeItemType.ResetState,
            ExpectedEncodeStart = expectedStart,
        });

        Assert.Same(item, Assert.Single(fixture.Manager.Queue));
        Assert.Equal(ProcMode.CMCheck, item.Mode);
        Assert.Equal(QueueState.LogoPending, item.State);
        Assert.Equal(DateTime.MinValue, item.EncodeStart);
        Assert.Equal("LogoPending", Assert.Single(fixture.Store.GetQueueView(null!).Items).State);
    }

    [Theory]
    [InlineData(QueueState.Complete, false)]
    [InlineData(QueueState.Queue, true)]
    [InlineData(QueueState.LogoPending, true)]
    [InlineData(QueueState.Encoding, true)]
    public async Task 条件付きリトライは完了時刻不一致または実行中なら変更しない(QueueState state, bool changeStart)
    {
        var fixture = new RestQueueTestFixture();
        var currentStart = new DateTime(2026, 10, 28, 12, 30, 0, DateTimeKind.Utc);
        var item = RestQueueTestFixture.Item(7, state);
        item.EncodeStart = changeStart ? currentStart.AddMinutes(1) : currentStart;
        await fixture.Seed(item);
        var expectedStart = changeStart ? currentStart : currentStart.AddMinutes(1);

        await fixture.Manager.ChangeItem(new ChangeItemData
        {
            ItemId = item.Id,
            ChangeType = ChangeItemType.ResetState,
            ExpectedEncodeStart = expectedStart,
        });

        Assert.Equal(state, item.State);
        Assert.Equal(item.EncodeStart, changeStart ? currentStart.AddMinutes(1) : currentStart);
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
        Assert.Contains("キュー状態が変化", fixture.Store.GetLastOperationResult().Message);
    }

    [Theory]
    [InlineData(-1)]
    [InlineData(int.MinValue)]
    public async Task 負の単一移動位置は通知せずキューを変更しない(int position)
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1), RestQueueTestFixture.Item(2));
        var before = fixture.Store.GetQueueView(null!);
        var original = fixture.Manager.Queue.ToArray();
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = 1, ChangeType = ChangeItemType.Move, Position = position });
        Assert.Equal(original, fixture.Manager.Queue);
        var after = fixture.Store.GetQueueView(null!);
        Assert.Equal(before.Version, after.Version);
        Assert.Equal(before.Digest, after.Digest);
        Assert.Null(fixture.Store.GetLastOperationResult());
    }

    [Fact]
    public async Task 範囲外の単一移動はキューを変更しない()
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1));
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = 1, ChangeType = ChangeItemType.Move, Position = 1 });
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
        Assert.Equal(1, Assert.Single(fixture.Manager.Queue).Id);
    }

    [Theory]
    [InlineData(QueueState.Queue)]
    [InlineData(QueueState.LogoPending)]
    [InlineData(QueueState.Encoding)]
    public async Task アクティブ項目のキャンセルは状態と通知を更新する(QueueState state)
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1, state));
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = 1, ChangeType = ChangeItemType.Cancel });
        Assert.Equal(QueueState.Canceled, Assert.Single(fixture.Manager.Queue).State);
        Assert.Equal("Canceled", Assert.Single(fixture.Store.GetQueueView(null!).Items).State);
        Assert.False(fixture.Store.GetLastOperationResult().IsFailed);
    }

    [Fact]
    public async Task 完了項目のキャンセルは状態を変えず拒否する()
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Seed(RestQueueTestFixture.Item(1, QueueState.Complete));
        await fixture.Manager.ChangeItem(new ChangeItemData { ItemId = 1, ChangeType = ChangeItemType.Cancel });
        Assert.Equal(QueueState.Complete, Assert.Single(fixture.Manager.Queue).State);
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task 出力なしまたは空の出力先は登録を拒否する(bool blank)
    {
        var fixture = new RestQueueTestFixture();
        await fixture.Manager.AddQueue(new AddQueueRequest
        {
            Mode = ProcMode.Batch,
            Outputs = blank ? new List<OutputInfo> { new() { DstPath = " " } } : new List<OutputInfo>(),
        });
        Assert.Empty(fixture.Manager.Queue);
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
        Assert.Contains(blank ? "出力先ディレクトリが指定されていません" : "出力が1つもありません", fixture.Store.GetLastOperationResult().Message);
    }

    [Theory]
    [InlineData(0, "元のキューアイテムが見つかりません")]
    [InlineData(1, "完了していないキューアイテムは再投入できません")]
    [InlineData(2, "CM解析またはエンコード済みのキューアイテムだけ再投入できます")]
    [InlineData(3, "入力ファイルが見つかりません")]
    public async Task 再投入の前提不足は追加前に拒否する(int id, string error)
    {
        var fixture = new RestQueueTestFixture();
        var wrongMode = RestQueueTestFixture.Item(2, QueueState.Complete);
        wrongMode.Mode = ProcMode.Test;
        var empty = RestQueueTestFixture.Item(3, QueueState.Complete);
        empty.SrcPath = "";
        await fixture.Seed(RestQueueTestFixture.Item(1), wrongMode, empty);
        var ex = await Assert.ThrowsAsync<InvalidOperationException>(() =>
            fixture.Manager.RequeueTrimItem(id, "標準", 3, new List<string>(), "/架空の一時", false));
        Assert.Equal(error, ex.Message);
        Assert.Equal(3, fixture.Manager.Queue.Count);
    }

    [Fact]
    public async Task 強制リトライは同じIDとモードを保ち通常スケジューラへ登録しない()
    {
        var (fixture, item, expectedStart) = await CreateForceStartFixtureAsync();
        var forceStarted = new List<QueueItem>();
        fixture.SetForceStartHandler((startedItem, accepted) =>
        {
            forceStarted.Add(startedItem);
            accepted();
        });

        await fixture.Manager.ChangeItem(new ChangeItemData
        {
            ItemId = item.Id,
            ChangeType = ChangeItemType.ResetStateAndForceStart,
            ExpectedEncodeStart = expectedStart,
        });

        Assert.Same(item, Assert.Single(forceStarted));
        Assert.Same(item, Assert.Single(fixture.Manager.Queue));
        Assert.Equal(ProcMode.CMCheck, item.Mode);
        Assert.Equal(QueueState.Queue, item.State);
        Assert.Equal(DateTime.MinValue, item.EncodeStart);
        Assert.Equal(0, GetScheduledQueueItemCount(fixture));
        Assert.False(fixture.Store.GetLastOperationResult().IsFailed);
    }

    [Fact]
    public async Task 強制リトライ要求が同時に届いても強制開始は一度だけ()
    {
        var (fixture, item, expectedStart) = await CreateForceStartFixtureAsync();
        var forceStartCount = 0;
        fixture.SetForceStartHandler((_, accepted) =>
        {
            accepted();
            System.Threading.Interlocked.Increment(ref forceStartCount);
        });
        var request = new ChangeItemData
        {
            ItemId = item.Id,
            ChangeType = ChangeItemType.ResetStateAndForceStart,
            ExpectedEncodeStart = expectedStart,
        };

        await Task.WhenAll(
            Task.Run(() => fixture.Manager.ChangeItem(request)),
            Task.Run(() => fixture.Manager.ChangeItem(request)));

        Assert.Equal(1, forceStartCount);
        Assert.Equal(QueueState.Queue, item.State);
        Assert.Equal(DateTime.MinValue, item.EncodeStart);
        Assert.Equal(0, GetScheduledQueueItemCount(fixture));
    }

    [Fact]
    public async Task ForceStartが受理前に例外なら完了状態と全snapshot項目を復元する()
    {
        var (fixture, item, expectedStart) = await CreateForceStartFixtureAsync();
        item.Priority = 4;
        item.EncodeTime = TimeSpan.FromMinutes(17);
        item.FailReason = "元の失敗理由";
        item.AutoLogoResult = AutoLogoResultState.Success;
        item.AutoLogoLastMessage = "元のロゴ状態";
        item.AutoLogoQueued = true;
        item.AutoLogoInProgress = true;
        var originalProfile = item.Profile;
        var forceStartCount = 0;
        fixture.SetForceStartHandler((_, _) =>
        {
            forceStartCount++;
            var workerPool = RestQueueTestFixture.GetField(fixture.Server, "workerPool");
            RestQueueTestFixture.SetProperty(workerPool, "MaintenancePaused", true);
            throw new InvalidOperationException("更新の適用中のため開始できません");
        });

        await fixture.Manager.ChangeItem(new ChangeItemData
        {
            ItemId = item.Id,
            ChangeType = ChangeItemType.ResetStateAndForceStart,
            ExpectedEncodeStart = expectedStart,
        });

        Assert.Equal(1, forceStartCount);
        Assert.Same(originalProfile, item.Profile);
        Assert.Equal(4, item.Priority);
        Assert.Equal(QueueState.Complete, item.State);
        Assert.Equal(expectedStart, item.EncodeStart);
        Assert.Equal(TimeSpan.FromMinutes(17), item.EncodeTime);
        Assert.Equal("元の失敗理由", item.FailReason);
        Assert.Equal(AutoLogoResultState.Success, item.AutoLogoResult);
        Assert.Equal("元のロゴ状態", item.AutoLogoLastMessage);
        Assert.True(item.AutoLogoQueued);
        Assert.True(item.AutoLogoInProgress);
        Assert.Equal(0, GetScheduledQueueItemCount(fixture));
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
    }

    [Fact]
    public async Task ForceStart受理後のQueue状態で例外でもsnapshotを戻さない()
    {
        var (fixture, item, expectedStart) = await CreateForceStartFixtureAsync();
        item.AutoLogoResult = AutoLogoResultState.Failed;
        item.AutoLogoLastMessage = "前回の自動補完結果";
        item.AutoLogoQueued = true;
        item.AutoLogoInProgress = true;
        var forceStartCount = 0;
        fixture.SetForceStartHandler((_, accepted) =>
        {
            forceStartCount++;
            accepted();
            throw new InvalidOperationException("accepted before asynchronous RunItem");
        });

        await fixture.Manager.ChangeItem(new ChangeItemData
        {
            ItemId = item.Id,
            ChangeType = ChangeItemType.ResetStateAndForceStart,
            ExpectedEncodeStart = expectedStart,
        });

        Assert.Equal(1, forceStartCount);
        Assert.Same(item, Assert.Single(fixture.Manager.Queue));
        Assert.Equal(QueueState.Queue, item.State);
        Assert.Equal(DateTime.MinValue, item.EncodeStart);
        Assert.Equal(TimeSpan.Zero, item.EncodeTime);
        Assert.Equal(AutoLogoResultState.None, item.AutoLogoResult);
        Assert.Null(item.AutoLogoLastMessage);
        Assert.False(item.AutoLogoQueued);
        Assert.False(item.AutoLogoInProgress);
        Assert.Equal(0, GetScheduledQueueItemCount(fixture));
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
    }

    [Fact]
    public async Task 強制リトライのExpectedEncodeStart欠損は項目を変更しない()
    {
        var (fixture, item, _) = await CreateForceStartFixtureAsync();
        var originalProfile = item.Profile;
        var forceStartCount = 0;
        fixture.SetForceStartHandler((_, _) => forceStartCount++);

        await fixture.Manager.ChangeItem(new ChangeItemData
        {
            ItemId = item.Id,
            ChangeType = ChangeItemType.ResetStateAndForceStart,
        });

        Assert.Equal(QueueState.Complete, item.State);
        Assert.Same(originalProfile, item.Profile);
        Assert.NotEqual(DateTime.MinValue, item.EncodeStart);
        Assert.Equal(0, forceStartCount);
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
        Assert.Contains("完了時刻", fixture.Store.GetLastOperationResult().Message);
    }

    [Theory]
    [InlineData(QueueState.Queue, false)]
    [InlineData(QueueState.LogoPending, false)]
    [InlineData(QueueState.Encoding, false)]
    [InlineData(QueueState.Complete, true)]
    public async Task 強制リトライの状態または完了時刻が変化した場合は変更しない(QueueState state, bool mismatch)
    {
        var (fixture, item, expectedStart) = await CreateForceStartFixtureAsync(state);
        if (mismatch) item.EncodeStart = expectedStart.AddMinutes(1);
        var originalStart = item.EncodeStart;
        var originalProfile = item.Profile;
        var forceStartCount = 0;
        fixture.SetForceStartHandler((_, _) => forceStartCount++);

        await fixture.Manager.ChangeItem(new ChangeItemData
        {
            ItemId = item.Id,
            ChangeType = ChangeItemType.ResetStateAndForceStart,
            ExpectedEncodeStart = expectedStart,
        });

        Assert.Equal(state, item.State);
        Assert.Equal(originalStart, item.EncodeStart);
        Assert.Same(originalProfile, item.Profile);
        Assert.Equal(0, forceStartCount);
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
        Assert.Contains("キュー状態が変化", fixture.Store.GetLastOperationResult().Message);
    }

    [Theory]
    [InlineData(ProcMode.Batch)]
    [InlineData(ProcMode.AutoBatch)]
    [InlineData(ProcMode.Test)]
    public async Task 強制リトライはCM解析とTrim調整以外のモードを拒否する(ProcMode mode)
    {
        var (fixture, item, expectedStart) = await CreateForceStartFixtureAsync(mode: mode);
        var originalProfile = item.Profile;
        var forceStartCount = 0;
        fixture.SetForceStartHandler((_, _) => forceStartCount++);

        await fixture.Manager.ChangeItem(new ChangeItemData
        {
            ItemId = item.Id,
            ChangeType = ChangeItemType.ResetStateAndForceStart,
            ExpectedEncodeStart = expectedStart,
        });

        Assert.Equal(QueueState.Complete, item.State);
        Assert.Same(originalProfile, item.Profile);
        Assert.Equal(expectedStart, item.EncodeStart);
        Assert.Equal(0, forceStartCount);
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
        Assert.Contains("CM解析またはTrim調整", fixture.Store.GetLastOperationResult().Message);
    }

    [Fact]
    public async Task 保守停止中の強制リトライは項目を変更しない()
    {
        var (fixture, item, expectedStart) = await CreateForceStartFixtureAsync();
        var originalProfile = item.Profile;
        var workerPool = RestQueueTestFixture.GetField(fixture.Server, "workerPool");
        RestQueueTestFixture.SetProperty(workerPool, "MaintenancePaused", true);
        var forceStartCount = 0;
        fixture.SetForceStartHandler((_, _) => forceStartCount++);

        await fixture.Manager.ChangeItem(new ChangeItemData
        {
            ItemId = item.Id,
            ChangeType = ChangeItemType.ResetStateAndForceStart,
            ExpectedEncodeStart = expectedStart,
        });

        Assert.Equal(QueueState.Complete, item.State);
        Assert.Same(originalProfile, item.Profile);
        Assert.Equal(expectedStart, item.EncodeStart);
        Assert.Equal(0, forceStartCount);
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
        Assert.Contains("更新の適用中", fixture.Store.GetLastOperationResult().Message);
    }

    [Theory]
    [InlineData("profile")]
    [InlineData("service-unknown")]
    [InlineData("service-unregistered")]
    [InlineData("missing-logo")]
    public async Task 強制リトライのprofileまたは実行条件が不足する場合は項目を変更しない(string failure)
    {
        var (fixture, item, expectedStart) = await CreateForceStartFixtureAsync(
            serviceId: failure == "service-unknown" ? -1 : failure == "service-unregistered" ? 2 : 1,
            includeProfile: failure != "profile",
            profileRequiresLogo: failure == "missing-logo");
        var originalProfile = item.Profile;
        var originalPriority = item.Priority;
        var forceStartCount = 0;
        fixture.SetForceStartHandler((_, _) => forceStartCount++);
        if (failure == "service-unregistered")
        {
            RestQueueTestFixture.SetProperty(fixture.Server, "AppData_", new EncodeServer.AppData
            {
                setting = new Setting(),
                services = new ServiceSetting { ServiceMap = new Dictionary<int, ServiceSettingElement>() },
            });
        }

        await fixture.Manager.ChangeItem(new ChangeItemData
        {
            ItemId = item.Id,
            ChangeType = ChangeItemType.ResetStateAndForceStart,
            ExpectedEncodeStart = expectedStart,
        });

        Assert.Equal(QueueState.Complete, item.State);
        Assert.Same(originalProfile, item.Profile);
        Assert.Equal(originalPriority, item.Priority);
        Assert.Equal(expectedStart, item.EncodeStart);
        Assert.Equal(0, forceStartCount);
        Assert.True(fixture.Store.GetLastOperationResult().IsFailed);
        Assert.NotEmpty(fixture.Store.GetLastOperationResult().Message);
    }

    [Fact]
    public void SharedとServerのChangeItemTypeは強制リトライ名をJSON往復できる()
    {
        var options = new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            Converters = { new JsonStringEnumConverter() },
        };
        var request = new Amatsukaze.Shared.ChangeItemData
        {
            ItemId = 7,
            ChangeType = Amatsukaze.Shared.ChangeItemType.ResetStateAndForceStart,
            ExpectedEncodeStart = new DateTime(2026, 10, 28, 12, 30, 0, DateTimeKind.Utc),
        };

        var json = JsonSerializer.Serialize(request, options);
        var parsed = JsonSerializer.Deserialize<ChangeItemData>(json, options);

        Assert.Contains("ResetStateAndForceStart", json);
        Assert.Equal(ChangeItemType.ResetStateAndForceStart, parsed?.ChangeType);
        Assert.Equal(request.ExpectedEncodeStart, parsed?.ExpectedEncodeStart);
    }

    [Fact]
    public async Task WorkerPoolはStartItem後RunItem完了前に所有権移譲を通知する()
    {
        var runGate = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var runFinished = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var worker = new TestScheduleWorker();
        var workerPool = new WorkerPool
        {
            Queue = new ScheduledQueue(),
            NewWorker = _ => worker,
            OnStart = () => runGate.Task,
            OnFinish = () =>
            {
                runFinished.TrySetResult(true);
                return Task.CompletedTask;
            },
            OnError = (_, _, _) => Task.CompletedTask,
        };
        workerPool.SetNumParallel(1);
        var item = RestQueueTestFixture.Item(19);
        item.Profile = new ProfileSetting { ReqResources = new ReqResource[8] };
        var accepted = false;

        workerPool.ForceStart(item, () => accepted = true);

        Assert.True(accepted);
        Assert.Equal(QueueState.Queue, item.State);
        Assert.False(worker.RunItemCalled);

        runGate.TrySetResult(true);
        await runFinished.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(worker.RunItemCalled);
    }

    private static async Task<(RestQueueTestFixture Fixture, QueueItem Item, DateTime ExpectedStart)> CreateForceStartFixtureAsync(
        QueueState state = QueueState.Complete,
        ProcMode mode = ProcMode.CMCheck,
        int serviceId = 1,
        bool includeProfile = true,
        bool profileRequiresLogo = false)
    {
        var fixture = new RestQueueTestFixture();
        var expectedStart = new DateTime(2026, 10, 28, 12, 30, 0, DateTimeKind.Utc);
        var profile = new ProfileSetting
        {
            Name = "標準",
            LastUpdate = DateTime.MinValue,
            NoLogoInCM = !profileRequiresLogo,
            NoDelogo = !profileRequiresLogo,
        };
        var item = RestQueueTestFixture.Item(7, state);
        item.Mode = mode;
        item.ServiceId = serviceId;
        item.EncodeStart = expectedStart;
        item.ProfileName = includeProfile ? profile.Name : "存在しないプロファイル";
        item.Profile = profile;

        RestQueueTestFixture.SetField(fixture.Server, "profiles", new Dictionary<string, ProfileSetting>
        {
            [profile.Name] = profile,
        });
        RestQueueTestFixture.SetProperty(fixture.Server, "PendingProfile", new ProfileSetting
        {
            Name = "プロファイル未選択",
            LastUpdate = DateTime.MinValue,
        });
        RestQueueTestFixture.SetProperty(fixture.Server, "AppData_", new EncodeServer.AppData
        {
            setting = new Setting(),
            services = new ServiceSetting
            {
                ServiceMap = new Dictionary<int, ServiceSettingElement>
                {
                    [1] = new ServiceSettingElement { ServiceId = 1, LogoSettings = new List<LogoSetting>() },
                },
            },
        });
        await fixture.Seed(item);
        return (fixture, item, expectedStart);
    }

    private static int GetScheduledQueueItemCount(RestQueueTestFixture fixture)
    {
        var scheduledQueue = RestQueueTestFixture.GetField(fixture.Server, "scheduledQueue");
        var buckets = (Dictionary<int, List<QueueItem>>[])RestQueueTestFixture.GetField(scheduledQueue, "queue");
        return buckets.Sum(bucket => bucket.Values.Sum(items => items.Count));
    }

    private sealed class TestScheduleWorker : IScheduleWorker
    {
        public bool RunItemCalled { get; private set; }

        public Task<bool> RunItem(QueueItem item, bool forceStart)
        {
            RunItemCalled = true;
            return Task.FromResult(true);
        }
    }
}
