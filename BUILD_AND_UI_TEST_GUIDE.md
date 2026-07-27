# Hướng dẫn build, chạy và kiểm tra UI GNIL

Tài liệu này dành cho việc kiểm tra các thay đổi ở Settings panel, Network panel, Dashboard, Media và Performance trong source tree hiện tại.

> GNIL chỉ chạy bên trong một phiên Niri đang hoạt động. Hãy thực hiện các lệnh chạy ứng dụng từ terminal mở trong Niri để có `NIRI_SOCKET` và `WAYLAND_DISPLAY` hợp lệ.

## 1. Lệnh nhanh nhất

Luồng mặc định để chỉnh và kiểm tra UI dùng profile `ui`: không build test,
không sinh debug symbols và dùng linker nhanh khi chạy trong Nix dev shell.

```bash
cd /home/imtraf/Projects/gnil
systemctl --user stop gnil.service
nix develop
just dev
```

`just dev` build `build-ui/gnil`, chạy foreground, theo dõi source/assets và tự
khởi động lại sau mỗi lần lưu. Watchexec gom các thay đổi trong 300 ms để một
lần lưu không tạo nhiều lượt build.

Mở terminal thứ hai để bật panel cần kiểm tra:

```bash
cd /home/imtraf/Projects/gnil
nix develop
export GNIL_CONFIG_HOME="$PWD/.dev/config"
export GNIL_STATE_HOME="$PWD/.dev/state"
./build-ui/gnil msg panel toggle settings
./build-ui/gnil msg panel toggle network
```

Khi kiểm tra xong, nhấn `Ctrl+C` ở terminal đang chạy GNIL. Nếu trước đó bạn dùng service hệ thống, bật lại bằng:

```bash
systemctl --user start gnil.service
```

## 2. Chuẩn bị môi trường

Kiểm tra đang ở trong phiên Niri:

```bash
echo "$NIRI_SOCKET"
echo "$WAYLAND_DISPLAY"
```

Hai giá trị không được rỗng. Nếu `NIRI_SOCKET` trống, hãy mở terminal từ bên trong Niri thay vì TTY hoặc SSH.

Kiểm tra xem một GNIL khác có đang chạy hay không:

```bash
systemctl --user status gnil.service
pgrep -a gnil
```

Không nên chạy đồng thời bản systemd và bản trong source tree. Dừng service trước khi test:

```bash
systemctl --user stop gnil.service
```

Vào development shell dạng tương tác:

```bash
cd /home/imtraf/Projects/gnil
nix develop
```

Các phần tiếp theo giả định terminal đang ở development shell. Nếu không muốn vào shell tương tác, thêm `nix develop -c` trước lệnh, ví dụ:

```bash
nix develop -c just build-app ui
```

## 3. Configure và build

### Profile UI nhanh

```bash
just configure ui
just build-app ui
just run ui
```

Profile này dùng `-O0`, giữ standard-library assertions nhưng tắt debug symbols,
test targets, LTO và unity build. `mold` được dùng với một worker để giảm thời
gian link và tránh lỗi parser trên object lớn; `ccache` giúp các lần build lại
sau khi đổi branch hoặc tạo lại build directory.

Số đo tham khảo trên máy hiện tại sau khi cache đã ấm:

- No-op build: khoảng 1 giây.
- Sửa một file UI, compile và link lại: khoảng 3,3 giây.
- Binary `build-ui/gnil`: khoảng 77 MB.

Kiểm tra profile, kích thước binary và cache:

```bash
just build-stats ui
```

### Build debug thông thường

```bash
just configure debug
just build debug
```

File chạy được tạo tại:

```text
build-debug/gnil
```

Kiểm tra binary:

```bash
./build-debug/gnil --version
```

### Build tăng dần sau khi sửa code

Khi chỉ cần chạy app, không cần build 39 test executables:

```bash
just build-app debug
just run debug
```

`just build debug` vẫn build toàn bộ target và phù hợp cho lượt validation trước
khi commit.

### Build sạch khi cache cũ gây lỗi

Lệnh sau xóa toàn bộ `build-debug` rồi configure/build lại:

```bash
just rebuild debug
```

Chỉ cần dùng khi Meson cache bị cũ, dependency thay đổi hoặc build tăng dần cho kết quả bất thường.

### Build release

```bash
just configure release
just build release
```

Binary release nằm tại `build-release/gnil`. Để debug UI và đọc log, nên ưu tiên bản `debug`.

## 4. Chạy bằng config thật

Luồng này đọc config người dùng tại `$XDG_CONFIG_HOME/gnil/settings.toml`, hoặc `~/.config/gnil/settings.toml` nếu `XDG_CONFIG_HOME` chưa đặt.

Terminal thứ nhất:

```bash
cd /home/imtraf/Projects/gnil
systemctl --user stop gnil.service
./build-debug/gnil
```

Terminal thứ hai:

```bash
cd /home/imtraf/Projects/gnil
./build-debug/gnil msg panel toggle settings
```

Mở Network panel:

```bash
./build-debug/gnil msg panel toggle network
```

Các panel liên quan khác:

```bash
./build-debug/gnil msg panel toggle dashboard
./build-debug/gnil msg panel toggle media
./build-debug/gnil msg panel toggle wallpaper
```

Gửi lại cùng một lệnh `toggle` để đóng panel đang mở.

## 5. Chạy bằng môi trường `.dev` cô lập

Luồng này không dùng config và state thật. Nó theo dõi source/assets, build tăng dần và tự khởi động lại GNIL:

```bash
cd /home/imtraf/Projects/gnil
systemctl --user stop gnil.service
nix develop -c just dev
```

`just dev` sử dụng:

```text
.dev/config
.dev/state
```

Để gửi IPC từ terminal thứ hai tới đúng instance `.dev`, phải đặt cùng các biến môi trường:

```bash
cd /home/imtraf/Projects/gnil
nix develop
export GNIL_ASSETS_DIR="$PWD/assets"
export GNIL_CONFIG_HOME="$PWD/.dev/config"
export GNIL_STATE_HOME="$PWD/.dev/state"

./build-ui/gnil msg panel toggle settings
./build-ui/gnil msg panel toggle network
```

Nếu bỏ các biến này, client có thể tìm socket của instance mặc định thay vì instance `.dev`.

## 6. Checklist kiểm tra Settings panel

Mở Settings:

```bash
./build-debug/gnil msg panel toggle settings
```

Kiểm tra lần lượt:

1. Sidebar trái hiển thị cả icon và text; mục đang chọn có nền màu primary nhẹ.
2. Hover từng icon phải hiện tooltip và có thể điều hướng bằng phím mũi tên.
3. Search nằm ở góc trên phải và trả về kết quả từ nhiều trang.
4. Appearance có hero `Wallpaper browser`.
5. Danh sách bên dưới có Theme, Font, Profile, Transition, Directories và Live.
6. Badge số lượng thay đổi theo các setting đang hiện; Theme có thể là 2 hoặc 3 tùy Dynamic Colours.
7. Nhấn một group phải mở trang con; nút Back và `Escape` quay lại đúng vị trí.
8. `Reset page` bị vô hiệu hóa khi trang không có override; khi có override phải yêu cầu xác nhận lần hai.
9. Nhấn `Wallpaper browser` phải chuyển sang wallpaper panel.
10. Thử cả light/dark theme và UI scale hiện tại của bạn.

## 7. Checklist kiểm tra Network panel

Mở Network:

```bash
./build-debug/gnil msg panel toggle network
```

Kết quả mong đợi:

1. Panel dùng kích thước Auto khoảng `620 × 640` logical pixels.
2. Card Current Connection nằm trọn trong panel.
3. Header Wi-Fi, nút VPN, refresh và toggle đều hiện.
4. Danh sách Wi-Fi nằm trong vùng scroll riêng và không bị đẩy ra ngoài viewport.
5. Chờ vài giây sau khi mở để lần scan đầu hoàn tất.
6. Có thể cuộn khi số lượng access point vượt chiều cao panel.
7. Network đang kết nối được đánh dấu và signal percentage cập nhật.
8. Network có mật khẩu mở password sheet; `Escape` hoặc click scrim đóng sheet.

### Kiểm tra NetworkManager nếu danh sách vẫn trống

```bash
systemctl status NetworkManager
nmcli general status
nmcli radio wifi
nmcli device status
nmcli device wifi list
```

Bật Wi-Fi và yêu cầu scan lại nếu cần:

```bash
nmcli radio wifi on
nmcli device wifi rescan
nmcli device wifi list
```

Nếu `nmcli device wifi list` cũng trống, vấn đề nằm ở NetworkManager, adapter, rfkill hoặc quyền hệ thống chứ không phải layout của GNIL.

Kiểm tra rfkill:

```bash
rfkill list
```

### Network panel vẫn sai chiều rộng

GNIL tôn trọng Custom width trong config. Tìm override hiện tại:

```bash
rg -n '\[settings\.panels\.network\]|width' "${XDG_CONFIG_HOME:-$HOME/.config}/gnil/settings.toml"
```

Override có dạng:

```toml
[settings.panels.network]
width = 480
```

Để dùng Auto width mới là 620, vào Settings → Panels → Panel sizes → Network width và chọn `Auto`, hoặc xóa riêng `width` của bảng Network bằng trình soạn thảo. Nên sao lưu trước khi sửa thủ công:

```bash
cp "${XDG_CONFIG_HOME:-$HOME/.config}/gnil/settings.toml" \
   "${XDG_CONFIG_HOME:-$HOME/.config}/gnil/settings.toml.bak"
```

Sau đó validate config:

```bash
./build-debug/gnil config validate "${XDG_CONFIG_HOME:-$HOME/.config}/gnil/settings.toml"
```

## 8. Chạy test và kiểm tra source

Chạy riêng route test của Settings:

```bash
just test debug nexus_route
```

Chạy riêng test lịch Night Light:

```bash
just test debug day_night_schedule
```

Chạy toàn bộ unit test:

```bash
just test debug
```

Kiểm tra whitespace trong diff:

```bash
git diff --check
```

Kiểm tra translation JSON:

```bash
python3 -m json.tool assets/translations/en.json >/dev/null
```

Xem các file đang thay đổi:

```bash
git status --short
git diff --stat
```

## 9. Xử lý lỗi build thường gặp

### Lỗi thứ tự designated initializer

Ví dụ:

```text
error: designator order for field ... does not match declaration order
```

C++ yêu cầu các field `.name = value` xuất hiện đúng thứ tự khai báo trong
struct props. Xem thứ tự trong header tương ứng dưới `src/ui/builders/` (hoặc
umbrella `src/ui/builders.h`), sửa initializer rồi build nhanh lại:

```bash
just build-app ui
```

### Meson chưa được configure

```bash
just configure debug
just build debug
```

### Dependency hoặc compile database bị cũ

Thử reconfigure trước:

```bash
just configure debug
just build debug
```

Nếu vẫn lỗi mới dùng build sạch:

```bash
just rebuild debug
```

### GNIL báo `NIRI_SOCKET is not set`

Xác nhận terminal đang chạy trong Niri:

```bash
echo "$NIRI_SOCKET"
```

Nếu chạy bằng systemd, import lại environment từ terminal trong Niri:

```bash
systemctl --user import-environment NIRI_SOCKET WAYLAND_DISPLAY XDG_CURRENT_DESKTOP
systemctl --user restart gnil.service
```

### IPC không kết nối được

Kiểm tra GNIL có đang chạy:

```bash
pgrep -a gnil
```

Nếu đang dùng `just dev`, đảm bảo terminal IPC đã export đúng `GNIL_CONFIG_HOME` và `GNIL_STATE_HOME` như mục 5.

## 10. Kết thúc phiên test

Dừng GNIL chạy từ source bằng `Ctrl+C`, sau đó khởi động lại service đã cài:

```bash
systemctl --user start gnil.service
systemctl --user status gnil.service
```

Nếu không dùng service systemd thì không cần bước này.
