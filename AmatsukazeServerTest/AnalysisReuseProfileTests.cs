using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Serialization;
using System.Text.Json;
using System.Xml.Linq;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class AnalysisReuseProfileTests
{
    [Fact]
    public void 新規プロファイルでは解析結果の再利用は無効()
    {
        Assert.False(ServerSupport.NormalizeProfile(null).EnableAnalysisReuse);
    }

    [Fact]
    public void 設定のない旧XMLプロファイルでは解析結果の再利用は無効()
    {
        var serializer = new DataContractSerializer(typeof(ProfileSetting));
        using var stream = new MemoryStream();
        serializer.WriteObject(stream, new ProfileSetting { EnableAnalysisReuse = true });
        stream.Position = 0;
        var document = XDocument.Load(stream);
        document.Root!.Elements().Single(element => element.Name.LocalName == "EnableAnalysisReuse").Remove();
        using var reader = document.CreateReader();
        var restored = Assert.IsType<ProfileSetting>(serializer.ReadObject(reader));

        Assert.False(ServerSupport.NormalizeProfile(restored).EnableAnalysisReuse);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void 解析結果の再利用設定はXML_JSON_DeepCopyで維持される(bool enabled)
    {
        var source = ServerSupport.NormalizeProfile(null);
        source.EnableAnalysisReuse = enabled;
        var serializer = new DataContractSerializer(typeof(ProfileSetting));
        using var stream = new MemoryStream();
        serializer.WriteObject(stream, source);
        stream.Position = 0;
        var fromXml = Assert.IsType<ProfileSetting>(serializer.ReadObject(stream));
        var fromJson = JsonSerializer.Deserialize<ProfileSetting>(JsonSerializer.Serialize(source));
        var copied = ServerSupport.DeepCopy(source);

        Assert.Equal(enabled, fromXml.EnableAnalysisReuse);
        Assert.NotNull(fromJson);
        Assert.Equal(enabled, fromJson.EnableAnalysisReuse);
        Assert.Equal(enabled, copied.EnableAnalysisReuse);
    }

    [Theory]
    [InlineData(ProcMode.Batch, false, false, false)]
    [InlineData(ProcMode.Batch, false, true, false)]
    [InlineData(ProcMode.Batch, true, false, true)]
    [InlineData(ProcMode.Batch, true, true, true)]
    [InlineData(ProcMode.AutoBatch, false, false, false)]
    [InlineData(ProcMode.AutoBatch, false, true, false)]
    [InlineData(ProcMode.AutoBatch, true, false, true)]
    [InlineData(ProcMode.AutoBatch, true, true, true)]
    [InlineData(ProcMode.CMCheck, false, false, false)]
    [InlineData(ProcMode.CMCheck, false, true, false)]
    [InlineData(ProcMode.CMCheck, true, false, true)]
    [InlineData(ProcMode.CMCheck, true, true, true)]
    [InlineData(ProcMode.TrimAdjust, false, false, true)]
    [InlineData(ProcMode.TrimAdjust, false, true, true)]
    [InlineData(ProcMode.TrimAdjust, true, false, true)]
    [InlineData(ProcMode.TrimAdjust, true, true, true)]
    public void モードとプロファイル設定で一時保持を制御し解析結果再利用とresume_dirは独立(ProcMode mode, bool noRemoveTmp, bool enabled, bool keepTmp)
    {
        const string resumeDir = @"C:\work\resume";
        const string resumeDatPath = @"C:\input\program.ts.resume.dat";
        var args = MakeArgs(mode, noRemoveTmp, enabled, resumeDir, resumeDatPath);

        Assert.Equal(enabled, args.Contains($"--resume-dat \"{resumeDatPath}\"", StringComparison.Ordinal));
        Assert.Contains($"--resume-dir \"{resumeDir}\"", args, StringComparison.Ordinal);
        Assert.Equal(keepTmp, args.Contains("--no-remove-tmp", StringComparison.Ordinal));
    }

    private static string MakeArgs(ProcMode mode, bool noRemoveTmp, bool enabled, string resumeDir, string resumeDatPath)
    {
        var server = (EncodeServer)RuntimeHelpers.GetUninitializedObject(typeof(EncodeServer));
        var setting = new Setting();
        var appData = new EncodeServer.AppData { setting = setting };
        typeof(EncodeServer)
            .GetProperty("AppData_", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(server, appData);

        return server.MakeAmatsukazeArgs(
            mode,
            new ProfileSetting { EnableAnalysisReuse = enabled, NoRemoveTmp = noRemoveTmp },
            setting,
            @"C:\work",
            false,
            @"C:\input\program.ts",
            string.Empty,
            @"C:\output\program.mp4",
            "{}",
            VideoStreamFormat.MPEG2,
            1,
            Array.Empty<string>(),
            false,
            string.Empty,
            string.Empty,
            string.Empty,
            string.Empty,
            string.Empty,
            resumeDir,
            resumeDatPath,
            @"C:\bat",
            string.Empty,
            string.Empty,
            0);
    }
}
