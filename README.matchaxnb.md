# matchaxnb's fork of moonlight-embedded

This is a friendly fork of moonlight-embedded with a twist.

Principles:

- we want to be able to merge back to [upstream](https://github.com/moonlight-stream/moonlight-embedded/) whenever
- features are prepared in branches named `feature/<something>` and are always based on `upstream/master`
- releases are prepared in branches named `release/<something>` by merging all the features we want
- tags are to be avoided: i'd like them to remain a privilege of upstream

Details:

- CI is managed through [that repo](https://github.com/matchaxnb/moonlight-embedded-packaging/).
- for simplicity, bullseye support is dropped there (specific packages not desired)
- goal is to get rid of VideoCore and to use ffmpeg's work instead (Broadcom VCore is evil)

## Contributing to this project

Start by forking the repository, it will be essential to all your work.

I prefer [conventional commits](https://www.conventionalcommits.org/en/v1.0.0/) so please use that if possible.

Release management is made by merging feature branches into the master branch.

### Add a new feature

```shell
# add upstream as a source
git remote add upstream git@github.com:moonlight-stream/moonlight-embedded.git && git fetch --all
# create a new branch called feature/<whatever> and work from the top of upstream/master
git checkout -b feature/quantum-leaps && git reset --hard upstream/master
# do your magic, and then commit using conventional commits
git add src/whatever && git commit -m "feat: implement quantum leaps"; git push -u feature/quantum-leaps
```

Then open a pull request targeting the branch `release/matchas-flavor` (i.e. default branch).

### Fix a bug on an existing feature branch

Sometimes we have bugs. In order to keep the changes focused, we fix them on the branches they occur. In that case, base your work on the feature branch in question and add a commit to it and then open a pull request targeting that feature branch.

### Extend an existing feature

If your work depends on an existing feature branch, create your own branch derived from the original feature branch. Your pull request should target the existing feature branch.

This way it's super easy to isolate this feature to offer it as a pull request to upstream afterwards.

If your feature is optional, you can put it behind compilation flags. In that case, please document them.
