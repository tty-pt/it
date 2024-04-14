# Introduction
This is based on [sem](https://github.com/quirinpa/sem). It is a tool for searching through an interval tree.
Meaning you can provide (START and STOP) events with corresponding ids, and you can search which ids were present at a given moment

# Building
Here's how to build on Alpine Linux:
```sh
make ALPINE=y
```
To build on other distributions you might have to provide "LIBDB\_PATH" in case the default doesn't work for you. Here's an example:
```sh
make LIBDB_PATH=$LIBDB_PATH
```
To build on OpenBSD, just "make" will be enough.

# Running
You can:
```sh
./it -?
```
To find out about the available options.
Otherwise, feed a file to sem:
```sh
./it "2023-04-01" < data.txt
```
This will show you what ids were present at that date.

If you give it a time interval:
```sh
./it "2023-04-01 2023-06-07" < data.txt
```
It will still show you which ids were present.

By the way, you can give it multiple arguments like that. Not just one.

If you give it the "-r" flag, it will only show you those that have always been present.

# Format

All dates should be in UTC ISO-8601 format, like this: "2022-03-21T08:40:23".
Optionally, we can have a date only, like "2022-02-01", this is assumed as "2022-01-31T24:00:00" or "2022-02-01T00:00:00".


Comments start with "#". It is assumed that the required items in the line are present before the "#", except in cases where there is a "#" at the start of a line.

## Participant starts
```
START <DATE> <ID>
```

## Participant stops
```
STOP <DATE> <ID>
```

# Dependencies
This program is dependant on libdb. On linux, it is also dependant on libbsd.

# Acknowledgements
Leon, thanks for your help debugging the program and for putting up with me.
