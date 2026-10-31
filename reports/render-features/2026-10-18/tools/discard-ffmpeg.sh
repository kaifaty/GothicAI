#!/bin/sh
# Stand-in for ffmpeg in the bench's video pipe: drop the raw frames so encoding never slows the game.
exec cat >/dev/null
