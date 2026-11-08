+++
title = "Privacy"
summary = "What this site and the project's YouTube uploader do with data."
+++
## This site

The site has no accounts, no cookies and no analytics. Its fonts are served by the site itself: it asks no font service. A short on a page
loads from youtube-nocookie.com only when you click it; from then on
[Google's privacy policy](https://policies.google.com/privacy) applies.

## GothicAI Shorts, the YouTube uploader

GothicAI Shorts is the project's command-line tool that uploads the project's own shorts to its YouTube
channel. The tool lives in the private `kaifaty/gothic-shorts` repository. Only the project's author uses
it, on the author's own computer; it is not offered as a service to anyone.

- It uses YouTube API Services. Using it means agreeing to the
  [YouTube Terms of Service](https://www.youtube.com/t/terms), and
  [Google's privacy policy](https://policies.google.com/privacy) applies.
- It asks Google for two permissions: to upload videos, and to read the signed-in channel and its videos.
- It reads the signed-in channel's id and name, to refuse to upload anywhere but the project's channel,
  and the privacy status of the video it has just uploaded. Nothing else is read.
- The sign-in token is kept in a file on that computer, readable only by its user, and is sent to Google
  only. No YouTube data is stored, shared or sold.
- Access can be revoked at any time on the
  [permissions page of the Google account](https://myaccount.google.com/permissions); deleting the token
  file removes it from the computer as well.

## Contact

Questions go to [the project's issues on GitHub](https://github.com/kaifaty/GothicAI/issues).
