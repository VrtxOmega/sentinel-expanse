import os
import subprocess
import hashlib
import tempfile
import sys
import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '../python')))
from sentinel_expanse import GodTierEngine, SentinelV6
from benchmark_harness import generate_structured_data

def get_file_hash(filepath):
    h = hashlib.sha256()
    with open(filepath, 'rb') as f:
        for chunk in iter(lambda: f.read(4096), b""):
            h.update(chunk)
    return h.hexdigest()

@pytest.fixture(scope="module")
def synthetic_data():
    fd, path = tempfile.mkstemp(suffix=".log")
    os.close(fd)
    generate_structured_data(path, size_mb=1)
    yield path
    if os.path.exists(path):
        os.remove(path)

def test_god_tier_roundtrip(synthetic_data):
    orig_hash = get_file_hash(synthetic_data)
    
    comp_fd, comp_path = tempfile.mkstemp(suffix=".god")
    os.close(comp_fd)
    decomp_fd, decomp_path = tempfile.mkstemp(suffix=".log")
    os.close(decomp_fd)
    
    try:
        eng = GodTierEngine()
        with open(synthetic_data, 'rb') as f_in, open(comp_path, 'wb') as f_out:
            eng.compress_stream(f_in, f_out)
            
        with open(comp_path, 'rb') as f_in, open(decomp_path, 'wb') as f_out:
            eng.decompress_stream(f_in, f_out)
            
        new_hash = get_file_hash(decomp_path)
        assert orig_hash == new_hash, "Hash mismatch for GodTierEngine"
    finally:
        for p in [comp_path, decomp_path]:
            if os.path.exists(p): os.remove(p)

def test_sentinel_v6_python_roundtrip(synthetic_data):
    orig_hash = get_file_hash(synthetic_data)
    
    comp_fd, comp_path = tempfile.mkstemp(suffix=".v6")
    os.close(comp_fd)
    decomp_fd, decomp_path = tempfile.mkstemp(suffix=".log")
    os.close(decomp_fd)
    
    try:
        eng = SentinelV6(dict_count=2, chunk_size=1024*1024)
        eng.compress(synthetic_data, comp_path)
        eng.decompress(comp_path, decomp_path)
            
        new_hash = get_file_hash(decomp_path)
        assert orig_hash == new_hash, "Hash mismatch for SentinelV6 (Python)"
    finally:
        for p in [comp_path, decomp_path]:
            if os.path.exists(p): os.remove(p)

def run_native_binary(binary_name, args):
    """Helper to run a native binary. On Windows, prefixes with 'wsl'."""
    cmd = []
    if os.name == 'nt':
        cmd.append('wsl')
        cmd.append('./' + binary_name)
    else:
        cmd.append('./' + binary_name)
    
    cmd.extend(args)
    cwd = os.path.abspath(os.path.join(os.path.dirname(__file__), '../c'))
    res = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"Command {' '.join(cmd)} failed with exit code {res.returncode}:\n{res.stderr}")

def _wsl_path(path):
    """Convert Windows path to WSL path if necessary"""
    if os.name == 'nt':
        drive, rest = os.path.splitdrive(path)
        drive = drive.strip(':').lower()
        rest = rest.replace('\\', '/')
        return f"/mnt/{drive}{rest}"
    return path

@pytest.mark.parametrize("version", ["v6", "v7"])
def test_native_roundtrip(synthetic_data, version):
    binary = f"sentinel_{version}"
    cwd = os.path.abspath(os.path.join(os.path.dirname(__file__), '../c'))
    
    comp_name = f"temp_comp_{version}.bin"
    decomp_name = f"temp_decomp_{version}.log"
    comp_path = os.path.join(cwd, comp_name)
    decomp_path = os.path.join(cwd, decomp_name)
    
    synthetic_wsl = _wsl_path(synthetic_data)
    
    try:
        run_native_binary(binary, ["compress", synthetic_wsl, comp_name, "2"])
        run_native_binary(binary, ["decompress", comp_name, decomp_name])
        
        orig_hash = get_file_hash(synthetic_data)
        new_hash = get_file_hash(decomp_path)
        assert orig_hash == new_hash, f"Hash mismatch for {binary}"
    finally:
        for p in [comp_path, decomp_path]:
            if os.path.exists(p): os.remove(p)
