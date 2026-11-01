### 脱离真机编译
- 如果你是个热心肠的小伙，打算帮别人编译补丁，可以按照这个步骤

### 从目标设备提取必要信息
- 在已root的待适配设备上使用命令提取重要信息

```sh
su
mkdir -p /sdcard/vivo_cms
cp /vendor_dlkm/lib/modules/vfcs-cms-core.ko /sdcard/vivo_cms/
cat /proc/config.gz > /sdcard/vivo_cms/config.gz
cd /sys/firmware/devicetree/base && tar cf /sdcard/vivo_cms/cmsdt.tar $(find . -name "cms_v*")
```

### 放置文件
- 将上面步骤里 /sdcard/vivo_cms 下生成的三个文件放到工作目录下，例如
- 手动放好文件，结构如：
- PD2505/vfcs-cms-core.ko
- PD2505/config.gz
- PD2505/cmsdt.tar

### 构建
```bash
py -3 -m pip install pyelftools
py -3 make-patch.py --from-dir ./PD2505
```
