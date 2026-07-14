using Meridian.Codex.Services;
using Xunit;

namespace Meridian.Codex.Tests;

public class RecentWorkspaceStoreTests
{
    [Fact]
    public void Recent_workspaces_persist_deduplicate_and_keep_most_recent_first()
    {
        var settings = Path.Combine(Path.GetTempPath(), "codex-recent-tests", Guid.NewGuid().ToString("N"), "recent.json");
        var store = new RecentWorkspaceStore(settings, capacity: 3);

        store.Add("/packs/one");
        store.Add("/packs/two");
        store.Add("/packs/one");

        var reopened = new RecentWorkspaceStore(settings, capacity: 3);
        Assert.Equal(new[] { Path.GetFullPath("/packs/one"), Path.GetFullPath("/packs/two") }, reopened.Load());
    }
}
