
######################################
##  make example

# Define variables
IP := $(shell ip addr show | grep -oP '(?<=inet\s)\d+(\.\d+){3}' | grep -v '127.0.0.1' \
	| tr '\n' ' ' | awk '{print $$1}')
LOOP_DEVICES := /dev/loop30 /dev/loop31 /dev/loop32
IMG_FILES := lustre-mgt.img lustre-mdt.img lustre-ost.img
MOUNT_POINTS := mgt mdt ost client

# Default target
example: check_ip configure mount

# Check IP address
check_ip:
	@if [ -z "$(IP)" ]; then \
		echo "empty ip_address"; \
		exit 1; \
	fi
	@cnt=$$(echo "$(IP)" | tr -s ' ' '\n' | wc -l); \
	if [ $$cnt -ne 1 ]; then \
		echo "too many ip_address"; \
		exit 1; \
	fi
	@echo "ip_address: $(IP)"

configure: build_loop mkfs


# Create loop devices image
build_images: 
	@echo "build images..."
	sudo dd if=/dev/zero of=lustre-mgt.img bs=1M count=256
	sudo dd if=/dev/zero of=lustre-mdt.img bs=2M count=1024
	sudo dd if=/dev/zero of=lustre-ost.img bs=2M count=1024
	if [ -d ost2 ] && [ ! -f lustre-ost_2.img ]; then \
		sudo dd if=/dev/zero of=lustre-ost_2.img bs=2M count=1024; \
	fi

# Set loop devices
build_loop:
	@echo "start set loop device..."
	sudo losetup /dev/loop30 lustre-mgt.img
	sudo losetup /dev/loop31 lustre-mdt.img
	sudo losetup /dev/loop32 lustre-ost.img
	if [ -d ost2 ] && [ -f lustre-ost_2.img ]; then \
		sudo losetup /dev/loop33 lustre-ost_2.img; \
	fi

remove_loop:
	@echo "remove loop device..."
	sudo losetup -d /dev/loop30 || true
	sudo losetup -d /dev/loop31 || true
	sudo losetup -d /dev/loop32 || true
	sudo losetup -d /dev/loop33 || true

# Create Lustre file systems
mkfs:
	@echo "start mkfs.lustre..."
	sudo mkfs.lustre --mgs /dev/loop30
	sudo mkfs.lustre --mdt --mgsnode=$(IP)@tcp --fsname=lustrefs --index=0 /dev/loop31
	sudo mkfs.lustre --ost --mgsnode=$(IP)@tcp --fsname=lustrefs --index=0 /dev/loop32
	if [ -d ost2 ] && [ -f lustre-ost_2.img ]; then \
		sudo mkfs.lustre --ost --mgsnode=$(IP)@tcp --fsname=lustrefs --index=1 /dev/loop33; \
	fi

remkfs:
	@echo "start mkfs.lustre..."
	sudo mkfs.lustre --mgs --reformat /dev/loop30
	sudo mkfs.lustre --mdt --mgsnode=$(IP)@tcp --reformat --fsname=lustrefs --index=0 /dev/loop31
	sudo mkfs.lustre --ost --mgsnode=$(IP)@tcp --reformat --fsname=lustrefs --index=0 /dev/loop32
	if [ -d ost2 ] && [ -f lustre-ost_2.img ]; then \
		sudo mkfs.lustre --ost --mgsnode=$(IP)@tcp --reformat --fsname=lustrefs --index=1 /dev/loop33; \
	fi

mkfs_ost2:
	sudo dd if=/dev/zero of=lustre-ost_2.img bs=2M count=1024
	sudo losetup /dev/loop33 lustre-ost_2.img
	sudo mkfs.lustre --ost --mgsnode=$(IP)@tcp --fsname=lustrefs --index=2 /dev/loop33

mount_ost2:
	@echo "start mount ost..."
	mkdir -p ost2
	sudo mount -t lustre /dev/loop33 ost2

remove_ost2:
	sudo umount /dev/loop33 || true

# Mount file systems
mount: mount_mgt mount_mdt mount_ost mount_client

mount_mgt:
	@echo "start mount mgt..."
	mkdir -p mgt
	sudo mount -t lustre /dev/loop30 mgt

mount_mdt:
	@echo "start mount mdt..."
	mkdir -p mdt
	sudo mount -t lustre /dev/loop31 mdt

mount_ost:
	@echo "start mount ost..."
	mkdir -p ost
	sudo mount -t lustre /dev/loop32 ost
	if [ -d ost2 ] && [ -f lustre-ost_2.img ]; then \
		sudo mount -t lustre /dev/loop33 ost2; \
	fi

mount_client:
	@echo "start mount client..."
	mkdir -p client
	sudo mount -t lustre $(IP)@tcp:/lustrefs client

# Clean up
remove:
	@echo "cleaning up..."
	sudo umount client || true
	sudo umount ost || true
	sudo umount ost2 || true
	sudo umount mdt || true
	sudo umount mgt || true
	sudo losetup -d /dev/loop30 || true
	sudo losetup -d /dev/loop31 || true
	sudo losetup -d /dev/loop32 || true
	sudo losetup -d /dev/loop33 || true
	#sudo rm -f lustre-mgt.img lustre-mdt.img lustre-ost.img

debug_mkfs:
	sudo losetup -d /dev/loop25 || true
	sudo dd if=/dev/zero of=lustre.img bs=1M count=64
	sudo losetup /dev/loop25 lustre.img
	sudo cp /usr/lib/lustre/mount_osd_ldiskfs.so /usr/lib/lustre/mount_osd_ldiskfs.so-backup
	sudo cp ../lustre/utils/mount_osd_ldiskfs.so /usr/lib/lustre/


debug_mount:
	sudo losetup -d /dev/loop25 || true
	sudo dd if=/dev/zero of=lustre.img bs=1M count=64
	sudo losetup /dev/loop25 lustre.img
	sudo mkfs.lustre --mgs /dev/loop25
	# @sudo mkfs.lustre --mdt --mgsnode=$(IP)@tcp --fsname=lustrefs --index=0 /dev/loop31
	# @sudo mkfs.lustre --ost --mgsnode=$(IP)@tcp --fsname=lustrefs --index=0 /dev/loop32

debug_clean:
	sudo umount mnt || true

mount_tmpfs:
	sudo mount -t lustre $(IP)@tcp:/lustrefs tmpfs

wbc:
	sudo lctl set_param 'llite.*.wbc.conf'="conf flush_mode=aging_keep max_inodes=2000000 size=16G"

wbc_batch:
	sudo lctl set_param 'llite.*.wbc.conf'="conf flush_mode=aging_keep flush_pol=batch max_batch_count=256 dirty_flush_thresh=256 max_inodes=2000000 size=16G"


.PHONY: check_ip build_images configure mkfs mount mount_mgt mount_mdt mount_ost mount_client remove

#####################################
