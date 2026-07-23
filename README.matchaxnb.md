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
