# lead

lead is a modal text editor for an interactive terminal. The executable is named `editor`.

## Build and install

On Linux, use a C11 compiler, Make, and the system C library. Run these commands from this directory.

```sh
make
./editor notes.txt
make install
```

Installation defaults to `$HOME/.local/bin/editor`. Override `PREFIX` for a different install location or set `DESTDIR` to stage an installation. Use `make clean` to remove the local executable.

## Basic commands

Press `i` to enter insert mode and Esc to return to normal mode. Use `:w` to save, `:w filename` to name an untitled buffer, or `:q` to quit.
