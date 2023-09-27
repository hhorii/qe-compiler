# (C) Copyright IBM 2023.
#
# This code is part of Qiskit.
#
# This code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions. You may obtain a copy of this license in the LICENSE.txt
# file in the root directory of this source tree.
#
# Any modifications or derivative works of this code must retain this
# copyright notice, and modified files need to carry a notice indicating
# that they have been altered from the originals.

"""
Unit tests for the compiler API using Aer simulator targets.
"""

import asyncio
from datetime import datetime, timedelta
import io
import os
import pytest

from qss_compiler import (
    compile_file,
    compile_file_async,
    compile_str,
    compile_str_async,
    InputType,
    OutputType,
    CompileOptions,
)
from qss_compiler.exceptions import QSSCompilationFailure

compiler_extra_args = ["--num-shots=1", "--enable-circuits=false"]


def check_mlir_string(mlir):
    assert isinstance(mlir, str)
    assert "module" in mlir
    assert "@aer_state" in mlir
    assert "@aer_state_configure" in mlir
    assert "@aer_allocate_qubits" in mlir
    assert "@aer_state_initialize" in mlir
    assert "@aer_state_finalize" in mlir


def test_compile_file_to_qem(example_qasm3_tmpfile, simulator_config_file):
    """Test that we can compile a file input via the interface compile_file
    to a QEM payload"""

    # To generate a QEM payload, $LD_PATH and $LIBAER_PATH has to be specified.
    # Currently qss-compiler does not have `libaer.so` so this test must be failed.
    # Also, in future, a single binary file will be generated for the aer-simulator target.
    with pytest.raises(QSSCompilationFailure):
        compile_file(
            example_qasm3_tmpfile,
            input_type=InputType.QASM3,
            output_type=OutputType.QEM,
            output_file=None,
            target="aer-simulator",
            config_path=simulator_config_file,
            extra_args=compiler_extra_args,
        )


def test_compile_str_to_qem(simulator_config_file, example_qasm3_str):
    """Test that we can compile an OpenQASM3 string via the interface
    compile_file to a QEM payload"""

    # To generate a QEM payload, $LD_PATH and $LIBAER_PATH has to be specified.
    # Currently qss-compiler does not have `libaer.so` so this test must be failed.
    # Also, in future, a single binary file will be generated for the aer-simulator target.
    with pytest.raises(QSSCompilationFailure):
        compile_str(
            example_qasm3_str,
            input_type=InputType.QASM3,
            output_type=OutputType.QEM,
            output_file=None,
            target="aer-simulator",
            config_path=simulator_config_file,
            extra_args=compiler_extra_args,
        )


def test_compile_file_to_mlir(example_qasm3_tmpfile, simulator_config_file):
    """Test that we can compile a file input via the interface compile_file
    to a MLIR file"""

    mlir = compile_file(
            example_qasm3_tmpfile,
            input_type=InputType.QASM3,
            output_type=OutputType.MLIR,
            output_file=None,
            target="aer-simulator",
            config_path=simulator_config_file,
            extra_args=compiler_extra_args + ["--aer-simulator-conversion"],
    )

    check_mlir_string(mlir)


def test_compile_str_to_mlir(example_qasm3_str, simulator_config_file):
    """Test that we can compile a file input via the interface compile_file
    to a MLIR file"""

    mlir = compile_str(
            example_qasm3_str,
            input_type=InputType.QASM3,
            output_type=OutputType.MLIR,
            output_file=None,
            target="aer-simulator",
            config_path=simulator_config_file,
            extra_args=compiler_extra_args + ["--aer-simulator-conversion"],
    )

    check_mlir_string(mlir)


def test_compile_file_to_mlir_file(
    example_qasm3_tmpfile, simulator_config_file, tmp_path
):
    """Test that we can compile a file input via the interface compile_file
    to a QEM payload into a file"""
    tmpfile = tmp_path / "output.mlir"

    result = compile_file(
        example_qasm3_tmpfile,
        input_type=InputType.QASM3,
        output_type=OutputType.MLIR,
        output_file=tmpfile,
        target="aer-simulator",
        config_path=simulator_config_file,
        extra_args=compiler_extra_args + ["--aer-simulator-conversion"],
    )
    
    # no direct return
    assert result is None
    file_stat = os.stat(tmpfile)
    assert file_stat.st_size > 0
    
    with open(tmpfile) as mlir_str:
        check_mlir_string(mlir_str)
        

def test_compile_str_to_mlir_file(
    example_qasm3_str, simulator_config_file, tmp_path
):
    """Test that we can compile a file input via the interface compile_file
    to a QEM payload into a file"""
    tmpfile = tmp_path / "output.mlir"

    result = compile_str(
        example_qasm3_str,
        input_type=InputType.QASM3,
        output_type=OutputType.MLIR,
        output_file=tmpfile,
        target="aer-simulator",
        config_path=simulator_config_file,
        extra_args=compiler_extra_args + ["--aer-simulator-conversion"],
    )
    
    # no direct return
    assert result is None
    file_stat = os.stat(tmpfile)
    assert file_stat.st_size > 0
    
    with open(tmpfile) as mlir_str:
        check_mlir_string(mlir_str)


async def sleep_a_little():
    await asyncio.sleep(1)
    return datetime.now()


@pytest.mark.asyncio
async def test_async_compile_str(simulator_config_file, example_qasm3_str):
    """Test that async wrapper produces correct output and does not block the even loop."""
    async_compile = compile_str_async(
        example_qasm3_str,
        input_type=InputType.QASM3,
        output_type=OutputType.MLIR,
        output_file=None,
        target="aer-simulator",
        config_path=simulator_config_file,
        extra_args=compiler_extra_args + ["--aer-simulator-conversion"],
    )
    # Start a task that sleeps shorter than the compilation and then takes a
    # timestamp. If the compilation blocks the event loop, then the timestamp
    # will be delayed further than the intended sleep duration.
    sleeper = asyncio.create_task(sleep_a_little())
    timestamp_launched = datetime.now()
    mlir = await async_compile
    timestamp_sleeped = await sleeper

    sleep_duration = timestamp_sleeped - timestamp_launched
    milliseconds_waited = sleep_duration / timedelta(microseconds=1000)
    assert (
        milliseconds_waited <= 1100
    ), f"sleep took longer than intended ({milliseconds_waited} ms instead of ~1000), \
        event loop probably got blocked!"

    check_mlir_string(mlir)


@pytest.mark.asyncio
async def test_async_compile_file(
    example_qasm3_tmpfile, simulator_config_file
):
    """Test that async wrapper produces correct output and does not block the even loop."""
    async_compile = compile_file_async(
        example_qasm3_tmpfile,
        input_type=InputType.QASM3,
        output_type=OutputType.MLIR,
        output_file=None,
        target="aer-simulator",
        config_path=simulator_config_file,
        extra_args=compiler_extra_args + ["--aer-simulator-conversion"],
    )
    # Start a task that sleeps shorter than the compilation and then takes a
    # timestamp. If the compilation blocks the event loop, then the timestamp
    # will be delayed further than the intended sleep duration.
    sleeper = asyncio.create_task(sleep_a_little())
    timestamp_launched = datetime.now()
    mlir = await async_compile
    timestamp_sleeped = await sleeper

    sleep_duration = timestamp_sleeped - timestamp_launched
    milliseconds_waited = sleep_duration / timedelta(microseconds=1000)
    assert (
        milliseconds_waited <= 1100
    ), f"sleep took longer than intended ({milliseconds_waited} ms instead of ~1000), \
         event loop probably got blocked!"

    check_mlir_string(mlir)
