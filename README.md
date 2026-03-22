# lc0-browser-bridge

Modified fork of [Leela Chess Zero (lc0)](https://github.com/LeelaChessZero/lc0) with a file-backed UCI transport mode for browser integration via the File System Access API.

## Overview

This adds a `fileuci` transport mode to lc0. Instead of reading UCI commands from stdin and writing responses to stdout, `fileuci` polls a shared directory and exchanges newline-delimited JSON files with a browser client.

The intended use case is a locally running native `lc0` process controlled by a web application through the File System Access API.

## Usage

```shell
./build/release/lc0 fileuci --weights=/absolute/path/to/network.pb
```

### Flags

```shell
--state-dir=/path/to/shared/lc0-dir
--poll-interval-ms=50
--weights=/absolute/path/to/network.pb
```

Default state directory: `/home/lucas/chess/lc0`

### Bridge files

- `browser-to-engine.ndjson`
- `engine-to-browser.ndjson`
- `engine-status.json`
- `session.json`

The standard stdin/stdout UCI mode is unchanged.

## Building

This is a fork of lc0 — all standard build instructions apply. See the [upstream documentation](https://github.com/LeelaChessZero/lc0#building-and-running-lc0) for detailed build instructions.

Quick start (Linux):

```shell
git clone --recurse-submodules https://github.com/chessceo/lc0-browser-bridge.git
cd lc0-browser-bridge
./build.sh
```

## License

GPL-3.0 — see [COPYING](COPYING). Based on [Leela Chess Zero](https://github.com/LeelaChessZero/lc0).
