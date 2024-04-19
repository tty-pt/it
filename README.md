# Introduction
This is based on [sem](https://github.com/quirinpa/sem). It is a tool for searching through an interval tree.
Meaning you can provide (START and STOP) events with corresponding ids, and you can search which ids were present at a given moment

# Running
Run itd (the daemon program):
```sh
sudo mkdir /var/lib/it
sudo ./itd
```

You can feed input to it:
```sh
cat sample.txt | ./it "2023-01-01"
```

And then just run it as:
```sh
./it "2023-01-01 2024-01-01"
```
This will show you what ids were present in that slice of time.

By the way, you can give it multiple arguments like that. Not just one.

# Flags
## itd
### -d
> detach
### -f FILENAME
> change default db filename
### -C DB\_HOME
> change default DB\_HOME (from "/var/lib/it")
### -S SOCK\_PATH
> change default SOCK\_PATH (from "/tmp/it-sock")
## it
### -S SOCK\_PATH
> change default SOCK\_PATH (from "/tmp/it-sock")
### -r QUERY
> query participants which are there the entire time
### -s QUERY
> get split information
### QUERY
> query participants

# Input Format

All dates should be in UTC ISO-8601 format, like this: "2022-03-21T08:40:23".
Optionally, we can have a date only, like "2022-02-01", this is assumed as "2022-01-31T24:00:00" or "2022-02-01T00:00:00".

Or, we can use long (or long long) signed integers.


Comments start with "#". It is assumed that the required items in the line are present before the "#", except in cases where there is a "#" at the start of a line.

## Participant starts
```
START <DATE> <ID>
```

## Participant stops
```
STOP <DATE> <ID>
```

# Building
This program is dependant on libdb. On linux, it is also dependant on libbsd. So make sure to:
```sh
sudo apt install libdb-dev libbsd-dev
```
Or equivalent.

Then you should just have to:
```sh
make
```

On Alpine Linux you might need to:
```sh
make ALPINE=y
```

# Acknowledgements
Leon, thanks for your help debugging the program and for putting up with me.
