#!/bin/bash
#
# SPDX-License-Identifier: MIT
#
# Copyright © 2020 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

export ASSEMBLY_SOURCE=./lib/i915/shaders/clear_kernel

function get_help {
        echo "Usage: ${0} [options]"
        echo "Note: hsw_clear_kernel.c/ivb_clear_kernel.c automatically generated by this script should never be modified - it would be imported to i915, to use as it is..."
        echo " "
        echo "Please make sure your Mesa tool is compiled with "-Dtools=intel" and "-Ddri-drivers=i965", and run this script from IGT source root directory"
        echo " "
        echo "Options are:"
        echo " -h                       display this help message, and exit"
        echo " -g=platform              generation of device: use "hsw" for gen7.5, and "ivb" for gen7 devices"
        echo " -o=name_of_file          output file to store Mesa assembled c-literal for the device - If none specified, default file will be used - ivb/hsw-cb_assembled"
        echo " -m=mesa                  Path to Mesa i965_asm binary"
        echo " "
        echo " Usage example: \"./scripts/generate_clear_kernel.sh -g hsw -o hsw_clear_buffer.h -m ~/mesa/build/src/intel/tools/i965_asm\""
}

function include_array # $1=array_name - update Mesa output with desired format
{
        array_declaration="static const u32 $(basename $1)_clear_kernel[] = {"
        close_array=";"
        sed -i "1s/.*/$array_declaration/" $output_file
        sed -i "$ s/$/$close_array/" $output_file
}
function prefix_header # $1=filename $2=comment
{
	cat <<EOF
// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 *
 * Generated by: IGT Gpu Tools on $(date)
 */

EOF
}

function check_output_file #check output file
{
        if [ "x$output_file" != "x" ]; then
                if [ -f "$output_file" ]; then
                        echo -e "Warning: The \"$output_file\" file already exist - choose another file\n"
                        get_help
                        exit 1
                fi
        else
                # It is okay to overwrite default file created
                echo -e "Output file not specified - using default file \"$gen_device-cb_assembled\"\n"
                output_file="$gen_device-cb_assembled"
        fi
}
function asm_cb_kernel # as-root <args>
{
        check_output_file

        # Using i965_asm tool to assemble hex file from assembly source
        $mesa_i965_asm -g $gen_device -t c_literal  $input_asm_source -o $output_file

        if [ ! -f ${output_file} ]; then
                echo -e "Failed to assemble CB Kernel with Mesa tool\n"
                get_help
                exit 1
        fi

        # Generate header file
        if [ "$gen_device" == "hsw" ]; then
                echo "Generating gen7.5 CB Kernel assembled file \"hsw_clear_kernel.c\" for i915 driver..."

                i915_filename=hsw_clear_kernel.c
                include_array $gen_device
                prefix_header > $i915_filename
                cat $output_file >> $i915_filename

        elif [ "$gen_device" == "ivb" ]; then
                echo "Generating gen7 CB Kernel assembled file \"ivb_clear_kernel.c\" for i915 driver..."

                i915_filename=ivb_clear_kernel.c
                include_array $gen_device
                prefix_header $gen_device > $i915_filename
                cat $output_file >> $i915_filename
        fi
}

while getopts "hg:o:m:" opt; do
	case $opt in
		h) get_help; exit 0;;
		g) gen_device="$OPTARG" ;;
		o) output_file="$OPTARG" ;;
                m) mesa_i965_asm="$OPTARG" ;;
		\?)
			echo -e "Unknown option: -$OPTARG\n"
			get_help
			exit 1
			;;
	esac
done
shift $(($OPTIND-1))

if [ "x$1" != "x" ]; then
	echo -e "Unknown option: $1\n"
	get_help
	exit 1
fi

if [ "x$mesa_i965_asm" == "x" ]; then
        echo -e "i965_asm binary not found\n"
        get_help
        exit 1
fi

if [ "x$gen_device" != "x" ]; then
        if [ "$gen_device" == "hsw" ]; then
                input_asm_source="${ASSEMBLY_SOURCE}/hsw.asm"
        elif [ "$gen_device" == "ivb" ]; then
                input_asm_source="${ASSEMBLY_SOURCE}/ivb.asm"
        else
                echo -e "Unknown platform specified\n"
                get_help
                exit 1
        fi
	asm_cb_kernel
else
        echo -e "Platform generation not specified\n"
        get_help
        exit 1
fi
