#/bin/bash

CVMFS_TEST_USER="sftnight"
CVMFS_SOURCE_DIR="$(pwd)/cvmfs"
VAGRANT_WORKSPACE="/vagrant"

# update package manager
apt-get update

# install required packages
apt-get install -y apache2 attr autofs autotools-dev bash cmake coreutils    \
                   curl debhelper debianutils fuse g++ gawk gcc gdb grep     \
                   gzip initscripts insserv libattr1-dev libc-bin libc6-dev  \
                   libfuse-dev libfuse2 libssl-dev make openssl patch perl   \
                   pkg-config psmisc python-dev sed sudo sysvinit-utils      \
                   unzip uuid uuid-dev zlib1g

# install some convenience packages
apt-get install -y git tig iftop htop jq screen python-unittest2

# install FakeS3 from rubygems
gem install fakes3

# drop a FakeS3 default configuration for CVMFS server
if [ ! -f /etc/cvmfs/fakes3.default.conf ]; then
  mkdir -p /etc/cvmfs
  cp ${VAGRANT_WORKSPACE}/vagrant/fakes3.default.conf /etc/cvmfs
fi

# link the CernVM-FS source directory in place
if [ ! -L $CVMFS_SOURCE_DIR ]; then
  ln -s $VAGRANT_WORKSPACE $CVMFS_SOURCE_DIR
  chown -h vagrant:vagrant $CVMFS_SOURCE_DIR
fi

# create CVMFS test user
if ! id $CVMFS_TEST_USER > /dev/null 2>&1; then
  useradd $CVMFS_TEST_USER
  usermod -a -G fuse $CVMFS_TEST_USER
  echo "$CVMFS_TEST_USER ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers
fi
