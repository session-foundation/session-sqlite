local submodules = {
  name: 'submodules',
  image: 'drone/git',
  commands: [
    'git fetch --tags',
    'git submodule update --init --recursive --depth=1 --jobs=4',
  ],
};

local apt_get_quiet = 'apt-get -o=Dpkg::Use-Pty=0 -q';

local docker_base = 'registry.oxen.rocks/';

local default_deps = ['g++', 'pkg-config', 'libsodium-dev', 'libfmt-dev'];

local debian_build(name,
                   image,
                   arch='amd64',
                   deps=default_deps,
                   build_type='Release',
                   cmake_extra='',
                   jobs=6)
      = {
  kind: 'pipeline',
  type: 'docker',
  name: name,
  platform: { arch: arch },
  steps: [
    submodules,
    {
      name: 'build & test',
      image: image,
      pull: 'always',
      commands: [
        'echo "Building on ${DRONE_STAGE_MACHINE}"',
        'echo "man-db man-db/auto-update boolean false" | debconf-set-selections',
        apt_get_quiet + ' update',
        apt_get_quiet + ' install -y eatmydata',
        'eatmydata ' + apt_get_quiet + ' dist-upgrade -y',
        'eatmydata ' + apt_get_quiet + ' install --no-install-recommends -y cmake make git ca-certificates ' + std.join(' ', deps),
        'mkdir build',
        'cd build',
        'cmake .. -DCMAKE_CXX_FLAGS=-fdiagnostics-color=always -DCMAKE_BUILD_TYPE=' + build_type +
        ' -DLOCAL_MIRROR=https://oxen.rocks/deps ' + cmake_extra,
        'make VERBOSE=1 -j' + jobs,
        './tests/testAll --colour-mode ansi -d yes',
      ],
    },
  ],
};

[
  debian_build('Debian sid', docker_base + 'debian-sid'),
]
