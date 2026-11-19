# KAM-Flow

**Control multiple Windows PCs with one keyboard, one mouse, and one headset — no extra cables or hardware switches required.**

KAM-Flow (Keyboard, Audio, Mouse) connects the Windows computers on your desk over your local network so they feel like a single machine. Slide your mouse off the edge of your main monitor and it seamlessly appears on your laptop or secondary PC. Audio from every connected PC plays through your main headset, your microphone can be shared back to secondary PCs for calls, your clipboard stays in sync, and files transfer with a simple drag-and-drop.

---

## Good to Know Before You Start

* **Same local network only:** All PCs must be connected to the same home or office network (Wi-Fi or Ethernet). KAM-Flow does not connect over the internet and never sends your data to cloud servers.
* **No screen mirroring:** KAM-Flow shares your keyboard, mouse, audio, microphone, clipboard, and files between PCs that each have their own physical screen. It is not a remote-desktop or screen-streaming app.
* **Windows 10 & 11:** Runs natively on Windows 10 and Windows 11.
* **One Server, one or more Clients:** Your main PC (with your physical keyboard, mouse, and headset) runs as the **Server**. One or more secondary PCs run as **Clients**. Each Client pairs with one Server at a time.
* **Portable:** No installer needed — just place `KAM-Flow.exe` in its own folder and run it as Administrator.

---

## Core Features

* **Seamless Mouse & Keyboard Sharing:** Move your cursor across screen edges to switch which PC you are controlling. Multi-monitor setups are supported on both the Server and Clients — the cursor moves naturally between your local monitors and only crosses to another PC at the outer desktop edge.
* **Centralized Audio Mixing:** Hear music, videos, or alerts from every connected Client through the headset or speakers plugged into your Server PC. Adjust each Client's volume or mute individual PCs right from the Server.
* **Microphone Broadcast:** Speak into the microphone connected to your Server PC and use it inside apps like Zoom, Discord, or Teams on a Client PC. *(Requires the free VB-CABLE driver on the Client — see [Sharing Your Microphone](#sharing-your-microphone-vb-cable-setup).)*
* **Game Mode:** Playing a full-screen game on your main PC? Press `Ctrl+Alt+G` to lock your mouse and keyboard to the Server so you never accidentally slide onto another screen or lose focus mid-game.
* **Instant Clipboard Sync:** Copy text on one PC and paste it on another automatically — no extra shortcuts needed.
* **Drag-and-Drop File Transfers:** Drop any file onto the KAM-Flow window to send it to another connected PC. Transfers run on a separate channel so your mouse, keyboard, and audio stay responsive.
* **Visual Screen Arrangement:** Match the on-screen **Spatial Layout** grid to how your monitors sit on your physical desk (left, right, above, or below).
* **Sleep, Wake & Safety Protection:** Automatically recovers after a PC wakes from sleep or if you unplug and reconnect a USB headset. If a connection drops, held keys and mouse buttons are released automatically so nothing gets stuck. If a Windows permission prompt (UAC), Lock Screen, or protected menu appears on a Client, control safely returns to the Server so you are never locked out.
* **Quiet Background Operation:** Minimizes to the Windows system tray and uses minimal background resources when idle.

---

## Privacy & Security

Everything sent between your PCs — keystrokes, mouse movements, audio, clipboard text, and files — stays strictly on your local network and is protected end-to-end:

* **Local-only:** KAM-Flow has no accounts, no telemetry, and no cloud servers. Nothing leaves your local network.
* **End-to-end encryption:** All traffic between your PCs is encrypted automatically — there is no "unencrypted mode" to accidentally turn on.
* **Safe PIN pairing:** The Server generates a random 8-digit **Master PIN**. Clients must enter it once to pair, the PIN is never sent in plain text over the network, and it is stored securely by Windows rather than in plain text files.
* **You stay in control:** Incoming file transfers always ask for your permission (**Accept** or **Decline**) before saving anything to disk. You can also regenerate your Master PIN at any time to disconnect all Clients immediately.

<details>
<summary><strong>Security Details (for power users)</strong></summary>

* **Encryption:** 256-bit AES-GCM authenticated encryption powered by Windows Cryptography Next Generation (CNG), protecting every packet and file chunk against eavesdropping and tampering.
* **Key & Credential Storage:** The 8-digit Master PIN is stored in the Windows Credential Manager (never in `kamflow.ini`) and verified through an encrypted handshake without traversing the network in plaintext.
* **Replay & Spoofing Protection:** Monotonic sequence checks, sliding-window anti-replay protection on UDP streams, session-bound UDP handshakes, and peer-IP verification on file transfer connections block replay and unauthorized local-network injection.
* **Safety Limits:** Strict message size bounds and automatic return-to-Server protection on Windows Secure Desktop / UAC prompts and elevated system menus prevent lockouts and memory abuse.

</details>

---

## Quick Start (5 Minutes)

1. **Download `KAM-Flow.exe`** from the Releases page and place it in its own folder (such as `C:\KAM-Flow\`) on each Windows PC.
2. **Start the Server (Main PC):** Right-click `KAM-Flow.exe` → **Run as Administrator**, click **START AS SERVER (Main PC)**, and click **Allow** if Windows Firewall asks. On the **Connections** tab, click **Reveal** to view your 8-digit **Master PIN**.
3. **Start the Client (Secondary PC):** Right-click `KAM-Flow.exe` → **Run as Administrator**, click **START AS CLIENT**, and allow it through Windows Firewall.
4. **Pair & Connect:** On the Client's **Connections** tab, find your Server under **Discovered Servers**, click **Pair**, enter the 8-digit Master PIN, and press **Enter** (or click **Pair**). Then click **Connect to [Server Name]**.
5. **Match Your Desk:** On the Server, open the **Spatial Layout** tab and drag your Client's box to match where that PC sits next to your main screen (left, right, above, or below). Slide your mouse across that edge — you are ready to go!

---

## Requirements & Installation

### Requirements
* **OS:** Windows 10 or Windows 11 on every PC
* **Network:** All PCs on the same local network (Wi-Fi or Ethernet)
* **Permissions:** Administrator privileges on each PC *(required by Windows so KAM-Flow can share mouse and keyboard input across all apps)*

### Setup
1. Create a folder on each PC — for example, `C:\KAM-Flow\` or a folder on your Desktop.
   * *Avoid placing it inside `C:\Program Files\`* because KAM-Flow saves your settings in a `kamflow.ini` file right next to `KAM-Flow.exe`.
2. Copy `KAM-Flow.exe` into that folder.
3. Right-click `KAM-Flow.exe` and choose **Run as Administrator**.
   * **Tip:** To avoid right-clicking every time, create a Desktop shortcut to `KAM-Flow.exe`, right-click the shortcut → **Properties** → **Shortcut** tab → **Advanced…** → check **Run as administrator** → click **OK**.
4. When Windows Defender Firewall pops up on first launch, click **Allow** so your PCs can find and communicate with each other on your local network.

---

## Using the Server (Main PC)

Run **Server** mode on the PC that has your physical keyboard, mouse, and headset connected.

At the top of the window, status badges always show whether your keyboard and mouse are controlling this PC (**LOCAL**, green) or a Client (**REMOTE**, orange), along with how many Clients are connected.

### 1. Connections Tab
* **Master PIN:** Automatically generated the first time you run the Server.
  * Click **Reveal** / **Hide** to show or mask the 8-digit PIN.
  * Click **Regenerate Master PIN** to generate a new PIN. Any connected Clients are disconnected immediately and will need to re-pair with the new PIN.
* **Control Status & Emergency Button:** Shows whether control is **LOCAL** or **REMOTE**. While controlling a Client, a **FORCE LOCAL CONTROL (Ctrl+Alt+M)** button appears so you can snap control back with a click or hotkey.
* **Connected Clients:** Lists every connected Client with its PC name, local IP address, and a **Disconnect** button.

### 2. Spatial Layout Tab
This tab shows a grid with your **Server** box (green) in the center and connected **Clients** (blue) around it. The PC you are currently controlling glows with an **[ACTIVE]** border.

```text
                    +------------+
                    |   Empty    |
                    +------------+
     +------------+ +------------+ +------------+
     |  Laptop-1  | |  Main-PC   | |   Empty    |
     |   Client   | |   Server   | |            |
     |            | |  [ACTIVE]  | |            |
     +------------+ +------------+ +------------+
                    +------------+
                    |   Empty    |
                    +------------+
```

* **Rearranging screens:** Click and drag any Client box onto an **Empty** slot (above, below, left, or right of the Server — or next to another Client) to match your physical desk. Dropping onto another Client swaps their positions. Your layout is saved automatically.
* **Corner Deadzone (%):** Protects the corners of your monitor (`0%` to `10%`, default `5%`) so moving to the Windows Start Menu or the top-right **X** close button does not accidentally jump your cursor to another PC. Click **Reset** to restore the default.

### 3. Preferences Tab
* **Startup Configuration**
  * **Default Start Mode:** Choose **Ask on Startup**, **Server**, or **Client** to skip the role-selection screen next time.
  * **Minimize to System Tray:** Hides KAM-Flow to the Windows notification area (next to the clock) when minimized. Left-click the tray icon, right-click it for **Show KAM-Flow** / **Exit**, or relaunch `KAM-Flow.exe` to restore the window directly to the foreground.
* **Input Synchronization**
  * **Enable Keyboard Control:** Share your keyboard when controlling a Client.
  * **Enable Clipboard Sync:** Automatically sync copied text between connected PCs.
  * **Enable File Transfers:** Allow sending and receiving files via drag-and-drop.
  * **Mouse Sensitivity:** Adjust how fast the cursor moves on Client screens (`0.10` to `3.00`, default `1.00`) without changing your Server's own mouse speed.
* **Audio Subsystem**
  * **Compress Audio with Opus (Wi-Fi Resilient):** *(Default: On)* Encodes network audio using low-latency 10 ms Opus with In-Band FEC (~24x smaller packets than raw PCM) to eliminate Wi-Fi IP fragmentation and recover single-packet drops cleanly. Connected Clients automatically synchronize their codec from the Server; when turned off (or on unsupported hardware rates), KAM-Flow uses MTU-safe chunked PCM (`<= 1280 B` per packet).
  * **Adaptive Jitter Buffer (Auto):** *(Default: On)* Automatically steps the active Server jitter target up (`+20 ms`, ceiling `200 ms`) during Wi-Fi bursts or underruns, then gently relaxes back down (`-10 ms` after 60 s of clean playback) to your **Min Jitter Buffer** floor without audible glitches. Turn off for a fixed manual buffer target.
  * **Min / Network Jitter Buffer:** Sets the buffer floor (or fixed target when Auto is off, `20 ms` to `200 ms`, default `80 ms`) with a live `(Active: XX ms)` readout when Adaptive mode is enabled. Changes apply smoothly live without reconnecting.
  * **Enable Master Audio Mix (Listen to Clients):** Plays audio received from your connected Clients through the Server's speakers or headset.
  * **Broadcast Server Microphone to Clients:** Sends your Server's default microphone to connected Clients (displays the active microphone name below the checkbox).
  * **Client Sync Toggles** *(visible when Master Audio Mix is enabled)*: Mute or unmute **Audio**, adjust each Client's individual **Vol** slider (`0.00` to `1.00`), or toggle **Clipboard** sync per Client.
* **Safety & Control**
  * **Emergency Hotkey (`Ctrl+Alt+M`):** Instantly pulls mouse and keyboard control back to the Server (customizable to `Ctrl+Alt+A` through `Ctrl+Alt+Z`).
  * **Enable Game Mode & Game Mode Toggle Hotkey (`Ctrl+Alt+G`):** Locks your mouse and keyboard to the Server PC so games never lose focus at screen edges.
* **Engine & Debug**
  * **Show Debug Console Tab:** Turns on the optional **Debug Log** tab for troubleshooting (uses no memory when turned off).

---

## Using a Client (Secondary PC)

Run **Client** mode on each secondary PC or laptop you want to control from your Server.

### 1. Connections Tab
* **Pairing to a Server (first time only):**
  1. Look under **Discovered Servers** — your Server should appear automatically within a few seconds.
  2. Click the green **Pair** button next to your Server.
  3. Type the Server's 8-digit **Master PIN** (click **Show** if you want to check your typing) and click **Pair** or press **Enter**.
  4. *Switching Servers:* A Client can pair with one Server at a time. Clicking **Pair** on a different Server shows a confirmation prompt asking to replace your current pairing.
* **Connecting & Disconnecting:**
  * Under **Currently Paired Server**, you will see your Server's name, IP address, and **[ONLINE]** or **[OFFLINE]** status.
  * Click **Connect to [Server Name]** to start the session.
  * While connected, click **Disconnect (`Ctrl+Alt+M`)** or press the emergency hotkey to disconnect immediately.
* **Edit IP:** If your router assigns your Server a new local IP address, click **Edit IP**, enter the new IPv4 address, and click **Save**. Your saved PIN stays intact — no need to re-pair.
* **Forget:** Click the red **Forget** button next to your paired Server under **Discovered Servers** to remove its saved PIN.

### 2. Spatial Layout Tab
* **Corner Deadzone (%):** Protects the corners of this Client's screen (`0%` to `10%`, default `5%`) from triggering accidental transitions back to the Server.

### 3. Preferences Tab
* **Startup Configuration:** Set **Default Start Mode**, **Minimize to System Tray**, and **Auto-Reconnect to Last Server** (automatically reconnects after brief drops or waking from sleep).
* **Input Synchronization:** Toggle **Enable Keyboard Control**, **Enable Clipboard Sync**, and **Enable File Transfers**.
* **Audio Subsystem:**
  * Audio compression (**Opus** vs **PCM**) is controlled automatically by the Server — no separate codec setting is needed on the Client.
  * **Network Jitter Buffer:** Adjust buffering (`20 ms` to `200 ms`, default `80 ms`) for incoming Server microphone audio.
  * **Send Client Audio to Server:** Streams this Client's system audio to your Server's headset.
  * **Receive Server Microphone:** Routes the Server's microphone into VB-CABLE on this Client.
* **Safety & Control:** Customize the **Emergency Hotkey (`Ctrl+Alt+M`)** to disconnect instantly.
* **Engine & Debug:** Toggle **Show Debug Console Tab** for troubleshooting.

---

## Sending & Receiving Files

Make sure **Enable File Transfers** is checked in **Preferences** on both PCs. One file transfer can run at a time.

1. **Send a file:** Drag any file from Windows Explorer and drop it directly onto the open KAM-Flow window.
2. **Confirm target:** A **File Transfer Offer** popup appears.
   * On the **Server**, check the box next to the Client(s) you want to send to, then click **Send**.
   * On a **Client**, click **Send** to offer the file to the Server.
3. **Accept on the receiving PC:**
   * An **Incoming File Transfer** popup appears in the bottom-right corner of the KAM-Flow window (if KAM-Flow is minimized to the tray, a Windows notification pops up; if it is in the background, the taskbar icon flashes).
   * Click **Accept** to choose where to save the file, or **Decline** to reject it.
4. **Track or cancel:** An **Active Transfers** progress bar appears in the bottom-right corner and a **Transfer: XX%** badge appears in the top bar. Click **X** beside the progress bar at any time to cancel a transfer (incomplete files are cleaned up automatically).

---

## Sharing Your Microphone (VB-CABLE Setup)

Windows does not allow regular apps to create virtual microphones by themselves. To let apps on a Client PC (such as Zoom, Discord, Teams, or OBS) hear the microphone plugged into your Server PC, install the free **VB-CABLE** driver on the **Client PC only**.

1. **Install VB-CABLE (Client PC only):**
   * Download **VB-CABLE Driver** from [vb-audio.com/Cable](https://vb-audio.com/Cable/).
   * Extract the downloaded `.zip` file.
   * Right-click `VBCABLE_Setup_x64.exe` → **Run as Administrator** → click **Install Driver**, then restart your PC if prompted.
2. **Restore your default Windows audio devices (Client PC only):**
   * Installing VB-CABLE often changes your default sound devices. Open **Windows Settings → System → Sound** (or right-click the speaker icon in the taskbar).
   * Set your **Output** device back to your physical speakers or headphones *(not `CABLE Input`)*.
   * Set your **Input** device back to your physical microphone *(not `CABLE Output`)*.
3. **Turn on microphone sharing in KAM-Flow:**
   * On the **Server PC:** Open **Preferences** and check **Broadcast Server Microphone to Clients**.
   * On the **Client PC:** Open **Preferences** and check **Receive Server Microphone**. *(Restart KAM-Flow on the Client if you just installed VB-CABLE.)*
4. **Select the microphone in your Client app:**
   * Open the audio settings of Zoom, Discord, Teams, or OBS on the Client PC and choose **CABLE Output (VB-Audio Virtual Cable)** as the microphone.

---

## Keyboard Shortcuts

| Shortcut (Default) | Where It Works | What It Does |
|--------------------|----------------|--------------|
| `Ctrl+Alt+M` | Server | **Emergency Override:** Instantly pulls mouse & keyboard control back to the Server PC. |
| `Ctrl+Alt+M` | Client | **Emergency Disconnect:** Immediately disconnects the Client from the Server. |
| `Ctrl+Alt+G` | Server | **Toggle Game Mode:** Locks or unlocks your cursor to the Server PC so games never lose focus. |

*(Both shortcuts can be changed to any `Ctrl+Alt+A` through `Ctrl+Alt+Z` combination in the **Preferences** tab.)*

---

## Troubleshooting

| Problem | What to Try |
|---------|-------------|
| **Server does not appear in Discovered Servers** | Make sure both PCs are on the same local network (not a guest Wi-Fi network that isolates devices) and that KAM-Flow is allowed through Windows Firewall on both PCs. If you have a VPN enabled on either PC, try pausing it or enabling local network access in the VPN settings. |
| **Client says "Authentication Failed" or wrong PIN** | The Server's Master PIN may have been regenerated or mistyped. On the Server's **Connections** tab, click **Reveal** to check the current 8-digit PIN. On the Client, click **Forget**, then click **Pair** and enter the new PIN. |
| **Cursor will not cross to the Client (or goes the wrong direction)** | 1. Confirm **Enable Game Mode** is **unchecked** on the Server.<br>2. Open the Server's **Spatial Layout** tab and check that the Client box sits on the side you are moving toward.<br>3. Move near the middle of the screen edge — corners are protected by the **Corner Deadzone** slider. |
| **Cursor feels too fast, too slow, or slightly laggy on the Client** | Adjust **Mouse Sensitivity** under Server **Preferences**. For the lowest possible latency (especially while gaming or transferring large files), connect your PCs via Ethernet or strong 5 GHz / 6 GHz Wi-Fi. |
| **Audio pops, clicks, or cuts out** | Ensure **Compress Audio with Opus (Wi-Fi Resilient)** and **Adaptive Jitter Buffer (Auto)** are checked in Server **Preferences → Audio Subsystem** (recommended on Wi-Fi), or raise the **Min / Network Jitter Buffer** slider toward `120–160 ms` (adjustments take effect smoothly live without reconnecting). Also verify **Enable Master Audio Mix** is checked on the Server and **Send Client Audio to Server** is checked on the Client. |
| **No sound from Client on Server (or Client audio is muted)** | Check **Preferences → Client Sync Toggles** on the Server to confirm that Client's **Audio** box is checked and its **Vol** slider is above `0.00`. |
| **Client apps do not hear the Server's microphone** | Verify **VB-CABLE** is installed on the Client, **Broadcast Server Microphone to Clients** is checked on the Server, **Receive Server Microphone** is checked on the Client, and your app's microphone is set to **CABLE Output (VB-Audio Virtual Cable)**. |
| **Mouse returns to Server unexpectedly when clicking a Client tray menu or admin popup** | This is intentional Windows security behavior. When a UAC prompt, Lock Screen, or protected system menu (such as the NVIDIA tray menu) opens on a Client, Windows blocks remote input to it, so KAM-Flow returns control to your Server to avoid locking you out. Use the Client's physical trackpad/keyboard to finish interacting with that prompt. |
| **Mouse or keyboard feels stuck after a crash or sudden disconnect** | Press `Ctrl+Alt+M` on the Server (or click **FORCE LOCAL CONTROL**). KAM-Flow also automatically releases stuck modifier keys and mouse buttons on disconnect. |
| **Drag-and-drop file transfer does nothing** | Check that **Enable File Transfers** is enabled on **both** PCs, that no other file transfer is currently running (one at a time), and that you are dropping the file directly onto the open KAM-Flow window. |
| **Server IP changed and Client shows [OFFLINE]** | On the Client's **Connections** tab, check the Server's new IP under **Discovered Servers**, click **Edit IP**, enter the new address, and click **Save**. |
| **Emergency or Game Mode hotkey does not work** | Another application on your PC may already be using `Ctrl+Alt+M` or `Ctrl+Alt+G`. Change the letter under **Preferences → Safety & Control**. |
| **KAM-Flow is running in the background but I cannot see the window** | Check the Windows system tray (`^` overflow area near the clock) and click the KAM-Flow icon, or launch `KAM-Flow.exe` again to bring the existing window back onto your active monitor. |

### Manual Firewall Ports (If Automatic Firewall Prompt Was Dismissed)

If you manage Windows Firewall or third-party firewall software manually, allow `KAM-Flow.exe` on Private networks — or open these local-network-only ports:

| Port | Protocol | Purpose |
|------|----------|---------|
| `39240` | TCP & UDP | Main encrypted connection (control, keyboard, mouse, clipboard, audio) |
| `39241` | UDP | Automatic local-network Server discovery |
| `39242` | TCP | Encrypted file transfers |

### Using the Built-in Debug Log

If an issue keeps happening and you want to see what KAM-Flow is doing:

1. Open the **Preferences** tab and check **Show Debug Console Tab**.
2. Open the new **Debug Log** tab that appears at the top.
3. Reproduce the issue. Messages are color-coded (red for errors/warnings, cyan for network, purple for audio, teal for performance summaries, yellow for security).
4. Use the **Filter** box to narrow down keywords (for example, `Audio` or `Network`), or click **Copy All** to copy the log text.
5. Uncheck **Show Debug Console Tab** when finished — KAM-Flow clears and frees the log buffer automatically.

---

<details>
<summary><strong>Building from Source (for developers)</strong></summary>

**Requirements:** Windows 10/11, CMake 3.15+, Visual Studio 2022+ (or Visual Studio Build Tools) with the **Desktop development with C++** workload (C++20 and Windows SDK). Dear ImGui and static libopus (BSD-3-Clause) are fetched automatically by CMake during configure.

Open a Visual Studio Developer Command Prompt in the repository root and run:

```bat
cmake -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

* Application binary: `build\Release\KAM-Flow.exe`
* Unit test binary: `build\Release\KAMFlowTests.exe`

</details>

---

## License

© KAM-Flow. All rights reserved.
