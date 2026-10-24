# Release notes

One file per release, `<version>.md`, with its **What's new**: a few short lines in players' words
(what they will notice, not how it was done). Write it before running the release workflow; the run
stops without it. `scripts/release-notes.py` puts the release's page together from three parts:

1. What's new: this release's file.
2. All changes, folded: the feat, fix and perf commits since the previous tag, from their
   Conventional Commits titles, and the compare link. Left out for the first release.
3. `footer.md`: the downloads (a table of the files the run built, with their sizes) and the install
   steps, the same every release.

The same What's new text is the AltStore / SideStore changelog and the Mac update window's notes.
