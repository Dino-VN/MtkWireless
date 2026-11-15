# MtkWireless

[English](README.md) · [Bản phát hành](https://github.com/Dino-VN/MtkWireless/releases)

[![Ủng hộ trên Ko-fi](https://img.shields.io/badge/Ko--fi-%E1%BB%A6ng%20h%E1%BB%99%20d%E1%BB%B1%20%C3%A1n-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/dinovn)

**Đến nhanh:** [Yêu cầu](#yêu-cầu) · [Tính năng](#những-gì-hoạt-động) · [Cài đặt](#cài-đặt) · [Xử lý lỗi](#kiểm-tra-kext-đã-nạp) · [Báo lỗi](#báo-lỗi) · [Mirroring](#iphone-mirroring-với-mirra) · [Build](#build-từ-mã-nguồn) · [Đóng góp](#ghi-nhận-đóng-góp) · [Giấy phép](#giấy-phép) · [Ủng hộ](#ủng-hộ)

Wi-Fi và Bluetooth cho card MediaTek MT7921 / MT7922 trên macOS 26 Tahoe
(OpenCore, Intel x86_64). Phần xử lý giao thức được viết bằng Rust; C++ chỉ
đảm nhiệm phần kết nối với IOKit, USB và Lilu.

Dự án cung cấp hai kext hoạt động độc lập với nhau:

| Kext | Chức năng | Phụ thuộc | Giấy phép |
|---|---|---|---|
| `MtkWirelessWifi.kext` | Wi-Fi PCIe qua ngăn xếp gốc của macOS (menu Wi-Fi, airportd, AWDL) | không có | GPL-2.0 |
| `MtkWirelessBT.kext` | Nạp firmware Bluetooth USB và patch `bluetoothd` để nhận controller MediaTek | Lilu, BlueToolFixup | GPL-3.0-only |

## Yêu cầu

- **macOS 26 Tahoe** (Darwin 25). Kext Wi-Fi không hoạt động trên Sequoia
  hoặc cũ hơn; phần mã hỗ trợ các phiên bản đó đã được bỏ.
- **OpenCore** trên máy Intel.
- Card được hỗ trợ:
  - Wi-Fi (PCI): `14c3:7961` MT7921, `14c3:0608` MT7921K / RZ608,
    `14c3:7922` MT7922, `14c3:0616` MT7922 / RZ616.
  - Bluetooth (USB): các ID MT7921 / MT7922 được liệt kê trong
    [`kext/BT/Info.plist`](kext/BT/Info.plist).
- Không dùng kext Wi-Fi hoặc Bluetooth khác cho cùng card (itlwm,
  AirportItlwm, IntelBluetooth\*, BrcmPatchRAM\*, các kext MediaTek khác),
  và không dùng SSDT vô hiệu hóa thiết bị Wi-Fi PCI.

## Những gì hoạt động

Các thử nghiệm bên dưới đều được thực hiện trên **một máy**: macOS 26.7.1
(25G241) và 26.7.2 (25G332), SMBIOS MacPro7,1, Wi-Fi MT7921 `14c3:7961`
với Bluetooth `0489:e0cd`, và iPhone 15 Pro Max (`iPhone16,2`) chạy iOS 18
và iOS 27. "Chưa kiểm chứng" nghĩa là đã có mã xử lý nhưng chưa ai xác nhận
hoạt động trên phần cứng thật.

### Theo bản build macOS

Bluetooth patch `bluetoothd` trong bộ nhớ, nên tính năng phụ thuộc vào đúng
bản build macOS. Kiểm tra bản của bạn bằng `sw_vers -buildVersion`.

| macOS | Wi-Fi, ghép đôi, âm thanh | AirDrop / Handoff (BLE advertising) | Micro băng rộng (HFP mSBC) | Instant Hotspot |
|---|---|---|---|---|
| 26.7.2 (25G332) | Hoạt động | Hoạt động | Hoạt động | Hoạt động |
| 26.7.1 (25G241) | Hoạt động | Hoạt động | Hoạt động | Không. Patch cần 25G332; hãy cập nhật lên 26.7.2 |
| 26.7.3 (25G342) | Chưa kiểm chứng | Chưa kiểm chứng | Chưa kiểm chứng | Chưa kiểm chứng |
| Các bản 26.x khác | Chưa kiểm chứng | Chưa kiểm chứng | Chưa kiểm chứng | Chưa kiểm chứng |

Trên 25G342, các patch (1.1.1+) khớp với binary `bluetoothd` trong thử
nghiệm offline, nhưng chưa chạy trên phần cứng thật. Trên mọi bản build,
kext kiểm tra từng signature trước khi patch và bỏ qua tính năng nếu kiểm
tra không đạt. Vì vậy, bản build không được hỗ trợ sẽ mất tính năng đó
thay vì làm `bluetoothd` crash.

### Wi-Fi

| Tính năng | Trạng thái | Ghi chú |
|---|---|---|
| Quét mạng, kết nối, DHCP | Hoạt động | AP WPA2 và WPA3-transition, 5 GHz kênh 149, 80 MHz, 802.11ac |
| AWDL | Hoạt động | Rời kênh AP sau khi bắt tay chế độ tiết kiệm điện |
| Kết nối mạng khi AirDrop đang hoạt động | Hoạt động | Quét và kết nối mạng được ưu tiên dùng radio thay AWDL cho đến khi liên kết với AP |
| WPA3-Personal (SAE), 802.11w PMF | Chưa kiểm chứng | SAE nhóm 19, chỉ hỗ trợ hunting-and-pecking. Không có H2E hoặc SAE-PK. `amtk_pmf=0` quay về chỉ dùng WPA2-PSK |
| 802.11ax | Chưa kiểm chứng | Mặc định được quảng bá ở 80 MHz, không có TWT / MU-MIMO / 160 MHz. `amtk_he=0` tắt tính năng này |
| 6 GHz (Wi-Fi 6E) | Chưa kiểm chứng | Tắt trừ khi có `amtk_6ghz=1`, chỉ dành cho MT7922 / MT7921K. Chưa thể kết nối vì 6 GHz cần H2E hoặc OWE |
| Kênh DFS (52–144) | Chưa kiểm chứng | Chỉ quét thụ động, không dùng cho AWDL |
| Roaming | Chưa kiểm chứng | Liên kết lại với AP cùng SSID, không phải 802.11r. `amtk_roam=0` tắt tính năng này |
| Sleep / wake | Chưa kiểm chứng | Khi thức dậy, HAL khởi động lại và nạp lại firmware nếu khởi động thất bại |

Chi tiết và toàn bộ boot-arg: [docs/WIFI.md](docs/WIFI.md),
[docs/ROAMING.md](docs/ROAMING.md), [docs/WIFI-6GHZ.md](docs/WIFI-6GHZ.md).

### Bluetooth

| Tính năng | Trạng thái | Ghi chú |
|---|---|---|
| Nạp firmware, ghép đôi | Hoạt động | Hiển thị `Chipset: THIRD_PARTY_DONGLE`. Cần các khóa NVRAM trong phần cài đặt |
| Âm thanh A2DP, AirPods (AAC-ELD) | Hoạt động | |
| Quét BLE | Hoạt động | Mac thấy iPhone |
| Quảng bá BLE | Hoạt động | iPhone thấy Mac. Cần 25G241 hoặc mới hơn |
| Micro HFP | Hoạt động | mSBC 16 kHz chỉ trên `0489:e0cd` và `13d3:3610`; các ID khác dùng CVSD băng hẹp. `-mtkbtnomsbc` buộc dùng CVSD |
| Mac kết nối tới iPhone qua BLE | Chưa kiểm chứng | |

Chi tiết và toàn bộ boot-arg: [docs/BLUETOOTH.md](docs/BLUETOOTH.md),
[docs/BLE-ADVERTISING.md](docs/BLE-ADVERTISING.md).

### Tính năng Apple

| Tính năng | Trạng thái | Ghi chú |
|---|---|---|
| AirDrop hai chiều, iOS 16–27 | Hoạt động | Trung bình khoảng 4 MB/s khi gửi iPhone → Mac. [Kết quả đo](docs/AIRDROP-PERFORMANCE.md) |
| Handoff, Universal Clipboard, AirPlay to Mac | Hoạt động | |
| Instant Hotspot (cùng tài khoản iCloud hoặc Family) | Hoạt động | 25G332 hoặc mới hơn. [Quá trình chẩn đoán](docs/INSTANT-HOTSPOT.md) |
| iPhone Mirroring | Thử nghiệm | Chỉ khi dùng [Mirra](https://github.com/eiyuumiru/Mirra), xem bên dưới |
| Auto Unlock, Sidecar, Continuity Camera / Microphone | Chưa kiểm chứng | |

## Cài đặt

Trước khi bắt đầu, hãy giữ một bản EFI hiện tại có thể khởi động trên USB.

### Script cài đặt

```sh
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Dino-VN/MtkWireless/main/scripts/install.sh)"
```

Script sẽ hỏi trước khi thay đổi và sao lưu `config.plist` cùng mọi kext bị
xóa vào `~/Desktop/MtkWireless-backup-<date>`. Script sẽ:

1. nhận diện phần cứng Wi-Fi và Bluetooth MediaTek, chỉ cài kext khớp với
   phần cứng;
2. tìm và mount phân vùng EFI mà OpenCore đã khởi động từ đó;
3. tải bản phát hành mới nhất hoặc cập nhật bản đã cài cũ hơn;
4. xóa các kext Intel, Broadcom và MediaTek xung đột khỏi `EFI/OC/Kexts`
   và `Kernel → Add`;
5. thêm Lilu và BlueToolFixup nếu thiếu, sửa thứ tự nạp và thiết lập các
   khóa NVRAM Bluetooth;
6. hiển thị những tính năng mà bản build macOS của bạn hỗ trợ.

Tùy chọn: `--dry-run` xem trước thay đổi, `--force` cài lại,
`--version v1.0.7` cài một bản phát hành cụ thể. Khởi động lại khi hoàn tất.

### Cài đặt thủ công

Tải `MtkWireless-<version>-RELEASE.zip` từ
[Bản phát hành](https://github.com/Dino-VN/MtkWireless/releases).

1. Mount phân vùng EFI mà OpenCore khởi động từ đó:

   ```sh
   diskutil list
   sudo diskutil mount disk0s1
   ```

2. Xóa các kext xung đột liệt kê trong phần [Yêu cầu](#yêu-cầu) khỏi
   `EFI/OC/Kexts` và `config.plist → Kernel → Add`.

3. Chép `MtkWirelessWifi.kext` và `MtkWirelessBT.kext` vào `EFI/OC/Kexts`.
   Với Bluetooth, bạn còn cần
   [Lilu](https://github.com/acidanthera/Lilu/releases) và
   `BlueToolFixup.kext` từ
   [BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM/releases).

4. Thêm các mục vào `Kernel → Add` theo thứ tự sau (hoặc gộp
   [docs/config-snippet.plist](docs/config-snippet.plist)):

   | # | BundlePath | ExecutablePath | MinKernel | MaxKernel |
   |---|---|---|---|---|
   | 1 | `Lilu.kext` | `Contents/MacOS/Lilu` | | |
   | 2 | `BlueToolFixup.kext` | `Contents/MacOS/BlueToolFixup` | `21.0.0` | |
   | 3 | `MtkWirelessWifi.kext` | `Contents/MacOS/MtkWirelessWifi` | `25.0.0` | `25.99.99` |
   | 4 | `MtkWirelessBT.kext` | `Contents/MacOS/MtkWirelessBT` | | |

   Với mỗi mục: `PlistPath` = `Contents/Info.plist`, `Arch` = `x86_64`,
   `Enabled` = true. Lilu phải đứng trước BlueToolFixup và MtkWirelessBT.

5. Với Bluetooth, thêm các khóa sau vào
   `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82`, và liệt kê cả
   hai khóa trong `NVRAM → Delete` của cùng GUID:

   | Khóa | Kiểu | Giá trị |
   |---|---|---|
   | `bluetoothInternalControllerInfo` | Data | `0000000000000000000000000000` (14 byte bằng 0) |
   | `bluetoothExternalDongleFailed` | Data | `00` |

   Nếu thiếu các khóa này, `bluetoothd` sẽ coi controller là module Broadcom
   của Apple và abort khi nhận phản hồi từ MediaTek.

6. Lưu, khởi động lại và chọn Reset NVRAM một lần trong menu OpenCore để
   áp dụng các khóa mới.

Để cập nhật, thay hai kext trong `EFI/OC/Kexts` rồi khởi động lại, hoặc chạy
lại script cài đặt.

### Kiểm tra kext đã nạp

```sh
kmutil showloaded --list-only | grep MtkWireless
ioreg -l -w0 | grep RustSelfTestFailed                   # expect 0
system_profiler SPBluetoothDataType | grep -E 'Chipset|State'
```

Bluetooth phải hiển thị `Chipset: THIRD_PARTY_DONGLE` và `State: On`, còn
menu Wi-Fi phải liệt kê được các mạng.

Trên máy mới, thử từng kext một. Bắt đầu với Bluetooth: ghép đôi thiết bị,
phát âm thanh, rồi thử AirDrop. Sau đó thêm Wi-Fi: kết nối mạng, rồi thử
AirDrop hai chiều.

| Hiện tượng | Cách xử lý |
|---|---|
| Không có giao diện Wi-Fi | Đảm bảo không có kext Wi-Fi khác nhận card, rồi kiểm tra lỗi nạp kext |
| Wi-Fi biến mất và khởi động lại vẫn không khôi phục | Firmware đã crash. Tắt máy, chờ 10–15 giây rồi bật lại |
| Bluetooth biến mất cùng Wi-Fi | Reset chip cũng làm Bluetooth mất kết nối. Khởi động lại |

## iPhone Mirroring với Mirra

[Mirra](https://github.com/eiyuumiru/Mirra) là dự án riêng giúp iPhone
Mirroring của Apple chạy trên Hackintosh Intel. MtkWireless chỉ cung cấp
kết nối Wi-Fi, Bluetooth và AWDL mà Mirra cần. Dự án không giả lập chip T2,
và Mirra không được đóng gói cùng các kext này.

Bạn cần:

- macOS 26.7.2 (25G332), bản build duy nhất đã được thử nghiệm;
- kext Wi-Fi build từ mã nguồn hiện tại (các zip phát hành cũ có thể thiếu
  những sửa lỗi RX và TX cần thiết);
- Mirra, thiết lập theo
  [hướng dẫn của dự án](https://github.com/eiyuumiru/Mirra#requirements);
- một **iPhone thứ hai đã jailbreak** (iOS 15+, OpenSSH, `ldid`) để Mirra
  dùng làm cầu nối. iPhone được phản chiếu không cần jailbreak.

Trạng thái hiện tại trên máy thử nghiệm: xác thực thành công, bộ giải mã
H.264 được tạo và khung hình video đầu tiên đã tới. Video ổn định, âm thanh,
điều khiển và kết nối lại chưa được kiểm chứng. Khi đang chạy, Mirra cũng
có thể làm hỏng AirDrop và các tính năng AWDL khác; chưa rõ nguyên nhân.
Ghi chú phía driver: [docs/IPHONE-MIRRORING.md](docs/IPHONE-MIRRORING.md).

## Báo lỗi

Chạy công cụ thu thập log **ngay sau khi lỗi xảy ra**, trước khi khởi động
lại Bluetooth hoặc bật/tắt Wi-Fi:

```sh
script="$(curl -fsSL https://raw.githubusercontent.com/Dino-VN/MtkWireless/main/scripts/collect-logs.sh)" && sudo bash -c "$script"
```

Công cụ tạo `MtkWireless-Logs-<date>-<random>.zip` trên Desktop. Gói log
chứa bản build macOS, thông tin card và kext, IORegistry, bộ đếm của driver,
trạng thái mạng, binary `bluetoothd`, các báo cáo crash gần đây và log
Wi-Fi / Bluetooth / AirDrop trong 5 phút gần nhất. Công cụ chỉ đọc: không
khởi động lại daemon, nạp kext, thay đổi cài đặt hoặc tải dữ liệu lên đâu.
Không cần Xcode hoặc Python.

Các tùy chọn hữu ích, thêm sau `--`:

```sh
sudo bash -c "$script" -- --last 15m --verbose --notes "AirDrop Mac to iPhone stalls"
```

| Tùy chọn | Tác dụng |
|---|---|
| `--last 15m` | Thu thập log trong khoảng thời gian dài hơn mặc định 5 phút |
| `--notes "..."` | Đính kèm mô tả lỗi |
| `--verbose` | Bao gồm log mức info/debug |
| `--logarchive` | Thêm log archive gốc của macOS (zip lớn hơn) |
| `--include-hci` | Chép các HCI trace có sẵn thay vì chỉ liệt kê |
| `--include-kexts` | Chép các gói kext |

**Gói log không được ẩn danh.** Nó có thể chứa SSID, địa chỉ MAC và IP,
số sê-ri, tên thiết bị và người dùng. Hãy gửi riêng; không đính kèm vào
issue công khai. Dữ liệu Keychain và khóa liên kết Bluetooth không được chép.

## Build từ mã nguồn

Cần Xcode và rustup.

```sh
scripts/bootstrap.sh          # MacKernelSDK submodule, Lilu SDK, pinned Rust nightly
scripts/build.sh Release      # build/Release/MtkWirelessWifi.kext, MtkWirelessBT.kext
scripts/package.sh Release    # dist/MtkWireless-<version>-RELEASE.zip
(cd rust && cargo test --workspace)
```

| Đường dẫn | Nội dung |
|---|---|
| `rust/mtk-wlan` | Wi-Fi: phân tích firmware, descriptor RX/TX, quét mạng, kết nối, SAE, AWDL |
| `rust/mtk-bt` | Bluetooth: firmware, WMT, sửa lệnh HCI, bộ phân giải patch `bluetoothd` |
| `rust/mtk-ffi`, `rust/mtk-kernel` | Giao diện C và runtime trong kernel cho mã Rust |
| `kext/Wifi` | Kext Wi-Fi; `vendor/` chứa HAL SimpleMtkWlan và net80211 của OpenBSD |
| `kext/BT` | Kext Bluetooth: bộ nạp USB, plugin Lilu, cầu nối mSBC |
| `firmware/` | Các firmware MediaTek được nhúng vào kext |
| `scripts/`, `tests/` | Build, cài đặt, chẩn đoán và kiểm thử hồi quy trên host |
| `docs/` | Ghi chú theo tính năng và log điều tra lỗi |

## Ghi nhận đóng góp

- [SimpleMtkWlan](https://github.com/laobamac/SimpleMtkWlan) của laobamac,
  xây dựng trên framework WLAN macOS của zxystd, với đóng góp từ Raboebie,
  mavizao và eiyuumiru. HAL Wi-Fi được lấy từ dự án này; xem
  [kext/Wifi/vendor/CREDITS.md](kext/Wifi/vendor/CREDITS.md).
- SimpleMtkBtFw của zxystd và laobamac, nền tảng của bộ nạp firmware
  Bluetooth.
- `net80211` và driver `if_mwx` của OpenBSD.
- libsbc của Google, dùng cho codec mSBC.
- [Lilu](https://github.com/acidanthera/Lilu),
  [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) và
  BlueToolFixup của acidanthera.
- [Mirra](https://github.com/eiyuumiru/Mirra) của eiyuumiru.
- Tất cả những người đã gửi patch và log.

## Giấy phép

`MtkWirelessWifi.kext` dùng GPL-2.0 và `MtkWirelessBT.kext` dùng GPL-3.0-only;
nội dung giấy phép nằm trong [LICENSES](LICENSES). Các tệp mã nguồn bên thứ ba
giữ nguyên phần ghi giấy phép riêng.

## Ủng hộ

Mình phát triển dự án trong thời gian rảnh và thử nghiệm trên phần cứng của
mình. Nếu các kext hữu ích với bạn, bạn có thể ủng hộ dự án qua Ko-fi:
<https://ko-fi.com/dinovn>
