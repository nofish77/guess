# Building

## Requirements

- A C compiler with C11 support
- POSIX threads on Linux/macOS
- Winsock2 on Windows

## Linux and macOS

Build the server through Make:

```bash
make
```

The equivalent direct command is:

```bash
cc -Wall -Wextra -pthread server.c -o server
```

Remove the generated executable with:

```bash
make clean
```

## Windows with MinGW

```powershell
gcc -Wall -Wextra server.c -o server.exe -lws2_32
```

## Windows with Visual Studio

Open a Developer Command Prompt and run:

```powershell
cl /W4 /utf-8 server.c ws2_32.lib
```

## Client

The original `client.c` source is not present in the repository history. The checked-in `client.exe` is retained temporarily for Windows users. Once the source is recovered, add a client build target and remove the executable from version control.

## Runtime defaults

- Server address used by the existing client: `127.0.0.1`
- Server port: `8888`
- Move timeout: 10 seconds
