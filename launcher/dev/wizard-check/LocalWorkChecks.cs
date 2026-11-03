// Checks of the guard that keeps the setup from deleting a working copy (LocalWork): scratch repositories made
// with the git on PATH, as the setup's own checkout looks (a clone of a tag, moved with checkout --detach) and as
// a developer's does (a branch, a worktree, an unpushed commit, a stash).
//   localwork <scratch>     <scratch> is emptied and filled with the repositories; needs git on PATH
using System.Diagnostics;
using MclaLauncher.Setup;

namespace MclaLauncher;

static partial class Program
{
    static void RunGitIn(string dir, params string[] args)
    {
        var psi = new ProcessStartInfo("git") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
        psi.ArgumentList.Add("-C"); psi.ArgumentList.Add(dir);
        foreach (var a in args) psi.ArgumentList.Add(a);
        psi.Environment["GIT_AUTHOR_NAME"] = psi.Environment["GIT_COMMITTER_NAME"] = "check";
        psi.Environment["GIT_AUTHOR_EMAIL"] = psi.Environment["GIT_COMMITTER_EMAIL"] = "check@example.invalid";
        using var p = Process.Start(psi)!;
        var err = p.StandardError.ReadToEnd();
        p.StandardOutput.ReadToEnd();
        p.WaitForExit();
        if (p.ExitCode != 0) throw new Exception($"git {string.Join(' ', args)} in {dir} failed: {err.Trim()}");
    }

    static async Task LocalWorkChecks(string scratch)
    {
        if (Directory.Exists(scratch)) SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);
        var origin = Path.Combine(scratch, "origin");
        Directory.CreateDirectory(origin);
        RunGitIn(origin, "init", "-q", "-b", "main");
        File.WriteAllText(Path.Combine(origin, "a.txt"), "one\n");
        RunGitIn(origin, "add", "a.txt");
        RunGitIn(origin, "commit", "-q", "-m", "one");
        RunGitIn(origin, "tag", "v1");
        File.WriteAllText(Path.Combine(origin, "a.txt"), "two\n");
        RunGitIn(origin, "commit", "-q", "-am", "two");
        RunGitIn(origin, "tag", "v2");

        async Task<List<string>> Find(string repo) => await LocalWork.FindAsync("git", null, repo, CancellationToken.None);
        string Clone(string name, string tag)
        {
            var dir = Path.Combine(scratch, name);
            RunGitIn(scratch, "-c", "advice.detachedHead=false", "clone", "-q", "--branch", tag, origin, dir);
            return dir;
        }

        // The setup's own checkout: a clone of a tag (detached, no branch of its own).
        var player = Clone("player", "v1");
        var found = await Find(player);
        Check("the setup's own checkout (a clone of a tag) holds no work", found.Count == 0, string.Join("; ", found));

        // After an update: checkout --detach of the next release, as Updates does.
        RunGitIn(player, "-c", "advice.detachedHead=false", "checkout", "-q", "--force", "--detach", "v2");
        found = await Find(player);
        Check("after an update (checkout --detach) it still holds none", found.Count == 0, string.Join("; ", found));

        // A changed tracked file in the setup's own checkout (a player's edit) is not a reason to stop: the setup keeps
        // its settings aside and has always replaced such a checkout.
        File.WriteAllText(Path.Combine(player, "a.txt"), "edited\n");
        found = await Find(player);
        Check("a player's edited file does not stop it", found.Count == 0, string.Join("; ", found));

        // A developer's working copy: on a branch, with a commit that is on no remote.
        var dev = Clone("dev", "v2");
        RunGitIn(dev, "switch", "-q", "-c", "work");
        File.WriteAllText(Path.Combine(dev, "b.txt"), "mine\n");
        RunGitIn(dev, "add", "b.txt");
        RunGitIn(dev, "commit", "-q", "-m", "mine");
        found = await Find(dev);
        Check("a working copy on a branch is found", found.Any(f => f.Contains("branch work")), string.Join("; ", found));
        Check("its unpushed commit is found", found.Any(f => f.Contains("1 commit on its branches is on no remote")), string.Join("; ", found));

        // On a branch that is pushed: still a working copy (the setup never leaves its own on a branch).
        var pushed = Clone("pushed", "v2");
        RunGitIn(pushed, "switch", "-q", "main");
        found = await Find(pushed);
        Check("a working copy on a pushed branch is found", found.Count == 1 && found[0].Contains("branch main"), string.Join("; ", found));

        // A worktree attached to an otherwise clean, detached checkout.
        var withWorktree = Clone("withwt", "v1");
        RunGitIn(withWorktree, "worktree", "add", "-q", "--detach", Path.Combine(withWorktree, ".claude", "worktrees", "side"), "v1");
        found = await Find(withWorktree);
        Check("a worktree inside it is found", found.Count == 1 && found[0].Contains("worktree"), string.Join("; ", found));

        // A stash in an otherwise clean, detached checkout.
        var stashed = Clone("stashed", "v1");
        File.WriteAllText(Path.Combine(stashed, "a.txt"), "changed\n");
        RunGitIn(stashed, "stash", "-q");
        found = await Find(stashed);
        Check("a stash is found", found.Count == 1 && found[0].Contains("stash"), string.Join("; ", found));

        // A detached checkout whose local branch holds unpushed commits (not checked out).
        var sideBranch = Clone("sidebranch", "v1");
        RunGitIn(sideBranch, "switch", "-q", "-c", "side");
        File.WriteAllText(Path.Combine(sideBranch, "c.txt"), "side\n");
        RunGitIn(sideBranch, "add", "c.txt");
        RunGitIn(sideBranch, "commit", "-q", "-m", "side");
        RunGitIn(sideBranch, "-c", "advice.detachedHead=false", "checkout", "-q", "--detach", "v1");
        found = await Find(sideBranch);
        Check("an unpushed branch that is not checked out is found", found.Count == 1 && found[0].Contains("on no remote"), string.Join("; ", found));

        // What an unfinished clone or a plain folder looks like: nothing to keep, replaced as before.
        var plain = Path.Combine(scratch, "plain");
        Directory.CreateDirectory(plain);
        File.WriteAllText(Path.Combine(plain, "x.txt"), "x");
        found = await Find(plain);
        Check("a folder without git holds no work", found.Count == 0, string.Join("; ", found));
        var broken = Path.Combine(scratch, "broken");
        Directory.CreateDirectory(Path.Combine(broken, ".git"));
        found = await Find(broken);
        Check("a folder whose .git git cannot read holds no work", found.Count == 0, string.Join("; ", found));
    }
}
