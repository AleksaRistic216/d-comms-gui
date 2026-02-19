# d-comms-gui

Desktop client for encrypted peer-to-peer messaging. No central server — peers discover each other automatically on the LAN or across the internet, and messages sync directly between them.

## Features

- End-to-end encrypted chat sessions
- Automatic LAN peer discovery; internet peers via DHT and UPnP
- Messages sync in the background every few seconds
- Per-sender message colouring

## Installation

### Requirements

- OpenGL-capable graphics driver
- GLFW (`libglfw3` on Debian/Ubuntu)

### Build from source

```bash
cmake -B build
cmake --build build
./build/dui
```

## Usage

### Create a chat

1. Click **New Chat** and enter a name.
2. Copy the credential string that appears and share it with the other person (e.g. via email or another messenger).

### Join a chat

1. Click **Join Chat**, enter a name, and paste in the credential string you received.
2. The chat opens immediately and messages sync in the background.

### Send messages

Type in the input field at the bottom and press **Enter** or click **Send**.

## Configuration

Internet connectivity works automatically on most routers via UPnP. If your router does not support UPnP, set `DCOMMS_HOST` to your public IP address so other peers can reach you:

```bash
DCOMMS_HOST=203.0.113.42 ./build/dui
```
