# Host Status Agent

This native C++17 program sends host CPU, memory, GPU, GPU memory and network statistics to the ESP32 clock over UDP port `9461`.

## Build

### Windows

Use Visual Studio or MinGW:

```powershell
cmake -S tools/host-agent -B tools/host-agent/build -G "Visual Studio 17 2022"
cmake --build tools/host-agent/build --config Release
```

With MinGW:

```powershell
cmake -S tools/host-agent -B tools/host-agent/build -G "MinGW Makefiles"
cmake --build tools/host-agent/build
```

### Linux

```bash
cmake -S tools/host-agent -B tools/host-agent/build
cmake --build tools/host-agent/build
```

### macOS

```bash
cmake -S tools/host-agent -B tools/host-agent/build
cmake --build tools/host-agent/build
```

## Run

Run in Command Prompt or PowerShell, from the build directory:

```text
host-status-agent.exe --esp-ip 10.4.212.47 --gpu auto --interval 1000
```

Options:

- `--esp-ip`: ESP32 LAN IP address.
- `--gpu auto|id`: GPU selection. `auto` selects the first discovered GPU.
- `--interval milliseconds`: Push interval, default `1000`.
- `--headless`: Disable terminal status text.
  - Default behavior keeps the console visible and prints live CPU, memory, GPU and network values.

The agent is a console application. It remains running until you press `Ctrl+C` or close the terminal. In Task Manager, look on the `Details` tab for `host-status-agent.exe`, not only the `Processes` app list.

The line-by-line console output is not required for background operation. To start it hidden on Windows:

```powershell
Start-Process -FilePath ".\host-status-agent.exe" `
  -ArgumentList "--esp-ip","10.4.212.47","--gpu","auto","--interval","1000","--headless" `
  -WindowStyle Hidden
```

`--headless` now suppresses live status text but keeps the console/control handler attached when started in a normal CMD/PowerShell console. You can stop it with `Ctrl+C`.

PowerShell execution policy may block `.ps1` scripts. Use the executable directly from CMD:

```bat
cd /d D:\Project\Esp32-Waveshare\tools\host-agent\build
host-status-agent.exe --esp-ip 10.4.212.47 --gpu auto --interval 1000
```

To hide the console from CMD, use the PowerShell launcher with execution-policy bypass:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\run-host-status-agent.ps1" -EspIp "10.4.212.47"
```

## GPU support

- Windows: NVIDIA GPU data is read through NVML when available.
- Linux: platform CPU, memory and network data are implemented. NVIDIA GPU provider support can be enabled with NVML.
- macOS: CPU, memory and network data are implemented. Apple GPU utilization and independent GPU memory are not guaranteed by public stable APIs and are not fabricated.

Multiple GPUs are enumerated on platforms that provide a GPU provider. Use `--gpu` with the numeric provider ID.

## UDP format

The agent sends an 80-byte big-endian packet every interval:

| Offset | Field |
|---|---|
| 0 | Magic `0x48535431` |
| 4 | Packet version `1` |
| 8 | Available fields flags |
| 12 | Sequence |
| 16 | Send time, reserved |
| 20 | CPU percent |
| 24 | Memory used MB |
| 28 | Memory total MB |
| 32 | GPU percent |
| 36 | GPU memory used MB |
| 40 | GPU memory total MB |
| 44 | Network receive KB/s |
| 48 | Network transmit KB/s |

The ESP32 hides the status panel after 10 seconds without a valid update.
