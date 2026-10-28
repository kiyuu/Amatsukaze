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
    [InlineData(false)]
    [InlineData(true)]
    public void 設定に応じてresume_datだけを制御しresume_dirは維持する(bool enabled)
    {
        const string resumeDir = @"C:\work\resume";
        const string resumeDatPath = @"C:\input\program.ts.resume.dat";
        var args = MakeBatchArgs(enabled, resumeDir, resumeDatPath);

        Assert.Equal(enabled, args.Contains($"--resume-dat \"{resumeDatPath}\"", StringComparison.Ordinal));
        Assert.Contains($"--resume-dir \"{resumeDir}\"", args, StringComparison.Ordinal);
    }

    private static string MakeBatchArgs(bool enabled, string resumeDir, string resumeDatPath)
    {
        var server = (EncodeServer)RuntimeHelpers.GetUninitializedObject(typeof(EncodeServer));
        var setting = new Setting();
        var appData = new EncodeServer.AppData { setting = setting };
        typeof(EncodeServer)
            .GetProperty("AppData_", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(server, appData);

        return server.MakeAmatsukazeArgs(
            ProcMode.Batch,
            new ProfileSetting { EnableAnalysisReuse = enabled },
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