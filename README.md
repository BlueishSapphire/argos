# argos

Argos is a small program that can watch files and run commands when the file emits an inotify event.

## Building

The shell script `make.sh` is both the build script and the install script.
It needs permission to access `/usr/local/bin` and `/usr/local/man/` so it can install the executable and man page.

```sh
$ git clone https://github.com/BlueishSapphire/argos.git
$ cd argos
$ sudo ./make.sh
```

## Running

### `argos [events... <command>] files...`

`files` is a list of files to watch.

`events... <command>` specifies to run `command` whenever one of the specified events is fired, where event is one of:

| Short name |  Long name |           inotify event(s) |
|------------|------------|----------------------------|
|       `-X` | `--all`    |                          * |
|       `-A` | `--access` |                     ACCESS |
|       `-M` | `--modify` |                     MODIFY |
|       `-O` | `--open`   |                       OPEN |
|       `-C` | `--create` |                     CREATE |
|       `-S` | `--close`  | CLOSE_WRITE, CLOSE_NOWRITE |
|       `-D` | `--delete` |                     DELETE |
|       `-B` | `--attrib` |                     ATTRIB |

## Examples

### Example 1

```sh
$ argos -M 'make' ./*.c
```

This command watches all files in the current directory with the ".c" extension and runs make when one of them gets modified.

This command has two main parts:
	1. `-M 'make'` specifies that when a file is modified (`-M`), it should run make (`'make'`).
	2. `./*.c` specifies to watch all ".c" files in the current directory

### Example 2

```sh
$ argos -M 'pandoc $file -o ${file##md}.pdf' ./*.md
```

This is a script that I often use while working on markdown files when I want a live preview. It waits for any markdown file to be modified, then uses pandoc (a document conversion utility) to convert the markdown file into a pdf.

This command has two main parts:
	1. `-M 'pandoc $file -o ${file##md}.pdf'` runs pandoc whenever one of the speciied files is modified (`$file` gets replaced with the name of the file, and `${file##md}` is some bash magic for removing the ".md" suffix from the filename)
	2. `./*.md` specifies to watch all markdown files in the current directory


