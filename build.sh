#!/bin/sh

# NOTE(rnp): to rebuild raylib run `touch build.sh` (or delete the so/dll)

cflags="-march=native -std=c11 -Wall -I./external/include"
#cflags="${cflags} -fsanitize=address,undefined"
#cflags="${cflags} -fproc-stat-report"
#cflags="${cflags} -Rpass-missed=.*"
libcflags="${cflags} -fPIC -shared -Wno-unused-variable"
ldflags="-lm"

cc=${CC:-cc}
build=release

for arg in $@; do
	case "$arg" in
	clang)   cc=clang      ;;
	gcc)     cc=gcc        ;;
	debug)   build=debug   ;;
	release) build=release ;;
	*) echo "usage: $0 [release|debug] [gcc|clang]"; exit 1;;
	esac
done

case $(uname -sm) in
MINGW64*)
	win32=1
	glfw="libglfw.dll"
	glfw_flags="-lgdi32 -lwinmm"
	raylib="libraylib.dll"
	main="main_w32.c"
	libname="beamformer.dll"
	ldflags="${ldflags} -lgdi32 -lwinmm"
	if [ ! ${NO_MATLAB} ]; then
		libcflags="${libcflags} -DMATLAB_CONSOLE"
		extra_ldflags="-llibmat -llibmex"
	fi
	${cc} ${libcflags} helpers/ogl_beamformer_lib.c -o helpers/ogl_beamformer_lib.dll \
		-L'C:/Program Files/MATLAB/R2022a/extern/lib/win64/microsoft' \
		${extra_ldflags}
	;;
Linux*)
	glfw="libglfw.so.3"
	glfw_flags="-D_GLFW_X11"
	raylib="libraylib.so"
	main="main_linux.c"
	libname="beamformer.so"
	cflags="${cflags} -D_DEFAULT_SOURCE"

	${cc} ${libcflags} helpers/ogl_beamformer_lib.c -o helpers/ogl_beamformer_lib.so
	;;
esac

if [ ! -f external/raylib/README.md ] || [ "$(git status --short external/raylib)" ]; then
	git submodule update --init --depth=1 external/raylib
fi

mkdir -p external/lib

build_raylib()
{
	cp external/raylib/src/rlgl.h external/include/
	cppflags="${2} -DPLATFORM_DESKTOP_GLFW -DGRAPHICS_API_OPENGL_43"
	cppflags="${cppflags} -Iexternal/raylib/src -Iexternal/raylib/src/external/glfw/include"

	case ${1} in
	shared)
		${cc} ${cflags} ${cppflags} -fPIC -shared -DBUILD_LIBTYPE_SHARED \
		        external/rcore_extended.c \
		        external/raylib/src/raudio.c external/raylib/src/rmodels.c \
		        external/raylib/src/rshapes.c external/raylib/src/rtext.c \
		        external/raylib/src/rtextures.c external/raylib/src/utils.c \
			-o ${raylib}
			;;
	static)
		${cc} ${cflags} ${cppflags} -c external/rcore_extended.c       -o external/lib/rcore.c.o
		${cc} ${cflags} ${cppflags} -c external/raylib/src/raudio.c    -o external/lib/raudio.c.o
		${cc} ${cflags} ${cppflags} -c external/raylib/src/rmodels.c   -o external/lib/rmodels.c.o
		${cc} ${cflags} ${cppflags} -c external/raylib/src/rshapes.c   -o external/lib/rshapes.c.o
		${cc} ${cflags} ${cppflags} -c external/raylib/src/rtext.c     -o external/lib/rtext.c.o
		${cc} ${cflags} ${cppflags} -c external/raylib/src/rtextures.c -o external/lib/rtextures.c.o
		${cc} ${cflags} ${cppflags} -c external/raylib/src/utils.c     -o external/lib/utils.c.o
		ar rc external/lib/libraylib.a external/lib/raudio.c.o external/lib/rcore.c.o \
		      external/lib/rmodels.c.o external/lib/rshapes.c.o external/lib/rtext.c.o \
		      external/lib/rtextures.c.o external/lib/rtextures.c.o external/lib/utils.c.o
		;;
	esac
}

check_and_rebuild_libs()
{
	# NOTE(rnp): we need to build glfw separately so that we can use functions from
	# glfw directly - raylib doesn't let us open multiple opengl contexts even if
	# we never plan on using them with raylib
	case "${1}" in
	static)
		if [ "./build.sh" -nt "${glfw}" ] || [ ! -f ${glfw} ]; then
			${cc} ${cflags} ${glfw_flags} -static  \
				-c external/raylib/src/rglfw.c -o external/lib/rglfw.o
			ar rc ${glfw} external/lib/rglfw.o
		fi
		;;
	shared)
		if [ "./build.sh" -nt "${glfw}" ] || [ ! -f "${glfw}" ]; then
			[ "${win32}" ] && glfw_flags="${glfw_flags} -D_GLFW_BUILD_DLL"
			${cc} ${cflags} ${glfw_flags} -fPIC -shared \
				external/raylib/src/rglfw.c -o ${glfw}
		fi
		;;
	esac
	if [ "./build.sh" -nt "${raylib}" ] || [ ! -f "${raylib}" ]; then
		[ ${1} = "static" ] && build_raylib ${1} "-static"
		[ ${1} = "shared" ] && build_raylib ${1} "-L. -lglfw ${ldflags}"
	fi
}

case "${build}" in
debug)
	cflags="${cflags} -O0 -D_DEBUG -Wno-unused-function"
	if [ "${win32}" ]; then
		# NOTE(rnp): export pdb on win32; requires clang
		cflags="${cflags} -fuse-ld=lld -g -gcodeview -Wl,--pdb="
	else
		cflags="${cflags} -ggdb -Wl,-rpath,."
	fi
	check_and_rebuild_libs "shared"
	ldflags="-L. -lglfw -lraylib ${ldflags}"
	${cc} ${cflags} -fPIC -shared beamformer.c -o ${libname} ${ldflags}
	;;
release)
	cflags="${cflags} -O3"
	raylib="external/lib/libraylib.a"
	glfw="external/lib/libglfw.a"
	ldflags="${raylib} ${glfw} ${ldflags}"
	check_and_rebuild_libs "static"
	;;
esac

${cc} ${cflags} -o ogl ${main} ${ldflags}
