# ApexOS: информация

### Простая x86 (i386) битная ОС


## Сборка и запуск
Перед началом убедитесь что у вас установлен весь софт для сборки ОС, либо перейдите в раздел releases в github и скачайте уже готовую скомпилированную систему

### 1. Установка софта

Выполните в терминале bash(или zsh) следующие команды в зависимости от вашего дистрибутива linux:

``` bash
# Debian/Ubuntu based
sudo apt update && sudo apt upgrade
sudo apt install gcc build-essential binutils grub-pc grub-common xorriso mtools git qemu-system-x86

# Arch based
sudo pacman -Syu
sudo pacman -S base-devel gcc grub xorriso mtools libisoburn git qemu-desktop

# Fedora/Redhat based
sudo dnf check-update
sudo dnf groupinstall "Development Tools"
sudo dnf install gcc binutils grub2-tools grub2-tools-extra xorriso mtools git qemu-system-x86

# Alpine based
apk update
apk add build-base gcc binutils make grub grub-bios xorriso mtools git qemu-system-x86_64
```

После этого проверьте установку:
```bash
make --version
ld --version      
grub-file --version
```

### 2. Компиляция
После установки софта выполните скачайте репозиторий а затем начните компляцию

```bash
git clone https://github.com/kataevilya/ApexOs.git
cd ApexOs
make && make run # ОС запуститься в ВМ qemu
```

Готово!


### Остальная информация

Мы очень рады людям которые хотят развивать проект, поэтому если вы хотите как то помочь проекту в развитии мы с радостью будем ждать от вас сообщение в телеграмм канал [от ryzik](https://t.me/canalik_rz)


