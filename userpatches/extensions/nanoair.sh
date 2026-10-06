# NanoAir image extension: enables the in-tree ac108_init module.
# Enable with: ENABLE_EXTENSIONS=nanoair

function custom_kernel_config__nanoair() {
	if [[ -f .config ]]; then
		# AC108 register-recipe loader (capture init only, no ASoC DAI).
		# Loaded at boot via modules-load.d; options mode=0 (H3 I2S master)
		# come from modprobe.d in the overlay.
		kernel_config_set_m CONFIG_SND_SUNXI_AC108_INIT
	fi
}
