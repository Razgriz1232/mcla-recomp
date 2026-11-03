// Whether a checkout the setup is about to delete holds someone's work: the setup only ever leaves its own
// checkout on a release commit (cloned with --branch <tag>, moved with checkout --detach), with no branch of
// its own, no worktrees and no stash. A folder that has any of those is a developer's working copy that
// happens to sit where the game goes (the setup was pointed at the folder the source was developed in), and
// deleting it would lose commits that exist nowhere else.
namespace MclaLauncher.Setup;

static class LocalWork
{
    /// What in `repo` would be lost, in plain words; empty when there is nothing (or when git cannot read the
    /// folder at all, which is what an unfinished clone looks like, and the setup replaces those as before).
    public static async Task<List<string>> FindAsync(string gitExe, string? gitDir, string repo, CancellationToken ct)
    {
        var found = new List<string>();
        if (!Directory.Exists(Path.Combine(repo, ".git")) && !File.Exists(Path.Combine(repo, ".git"))) return found;

        async Task<(int Code, List<string> Lines)> Git(params string[] args)
        {
            var lines = new List<string>();
            var code = await ChildProcess.RunAsync(gitExe, new[] { "-C", repo }.Concat(args), null, gitDir, lines.Add, ct);
            return (code, lines);
        }

        if ((await Git("rev-parse", "--git-dir")).Code != 0) return found;

        var branch = await Git("symbolic-ref", "-q", "--short", "HEAD");
        if (branch.Code == 0 && branch.Lines.FirstOrDefault()?.Trim() is { Length: > 0 } name)
            found.Add($"it is on the branch {name}, where the setup never leaves its own copy");

        var worktrees = await Git("worktree", "list", "--porcelain");
        if (worktrees.Code == 0)
        {
            var others = worktrees.Lines.Count(l => l.StartsWith("worktree ", StringComparison.Ordinal)) - 1;
            if (others > 0) found.Add(others == 1 ? "another working folder (a git worktree) belongs to it" : $"{others} other working folders (git worktrees) belong to it");
        }

        var unpushed = await Git("rev-list", "--count", "--branches", "--not", "--remotes", "--tags");
        if (unpushed.Code == 0 && int.TryParse(unpushed.Lines.FirstOrDefault()?.Trim(), out var n) && n > 0)
            found.Add(n == 1 ? "1 commit on its branches is on no remote" : $"{n} commits on its branches are on no remote");

        var stash = await Git("stash", "list");
        if (stash.Code == 0 && stash.Lines.Any(l => l.Trim().Length > 0))
            found.Add("it has stashed changes");

        return found;
    }
}
