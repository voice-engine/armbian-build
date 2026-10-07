# NanoAir image extension: enables the in-tree AC108 capture codec.
# Enable with: ENABLE_EXTENSIONS=nanoair

function custom_kernel_config__nanoair() {
	if [[ -f .config ]]; then
		# AC108 encoding-mode capture codec (sound/soc/codecs/ac108c.c,
		# board-verified). Loaded at boot via modules-load.d; the legacy
		# ac108_init loader is blacklisted (modprobe.d in the overlay) and
		# no longer built.
		kernel_config_set_y CONFIG_SND_SOC_AC108C	# 编译进内核: 开机即绑, 无需模块加载
	fi
}
