################################################################################
#
# Vagrantfile
#
################################################################################

### Change here for more memory/cores ###
VM_MEMORY=4096
VM_CORES=8

Vagrant.configure('2') do |config|
	config.vm.box = 'bento/ubuntu-20.04'
	config.vm.synced_folder ".", "/vagrant", type:"virtualbox"

	config.vm.provider :virtualbox do |v, override|
		v.memory = VM_MEMORY
		v.cpus = VM_CORES

		required_plugins = %w( vagrant-vbguest )
	end

	config.vm.provision 'shell' do |s|
		s.inline = 'echo Setting up machine name'

		config.vm.provider :virtualbox do |v, override|
			v.name = "Buildroot"
		end
	end

	config.vm.provision 'shell', privileged: true, inline:

		"
		apt-get -q update
		apt-get purge -q -y snapd lxcfs lxd ubuntu-core-launcher snap-confine
		apt-get -q -y install build-essential libncurses5-dev \
			git bzr cvs mercurial subversion unzip bc zstd
		apt-get -q -y autoremove
		apt-get -q -y clean
		update-locale LC_ALL=C


		"

	config.vm.provision 'shell', privileged: false, inline:

		"
		rsync --links -r /vagrant/ src
        cd src
        make galileo_defconfig
        BOARD_INTEL_DISK_IMAGE=yes make
        sudo bash -e inject-alpine.sh
        sudo bash -e inject-arch.sh
        cp output/images/sdcard.img /vagrant
        cp output/images/sdcard-arch.img /vagrant
        cp output/images/sdcard-alpine.img /vagrant
        echo Built buildroot  image ready at sdcard.img
        echo Built alpine image ready at sdcard-alpine.img
        echo Built arch image ready at sdcard-arch.img
		"


end
