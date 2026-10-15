"""Exercise a prepared musl tree on any Linux container with Python and Bash.

The supplied installation is copied to a path containing spaces and Unicode,
then moved again. Tests never change the source installation or host defaults.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve()
repo = Path(sys.argv[2]).resolve()
version = (repo / 'VERSION').read_text().strip()
subprocess.run(['bash', str(repo / 'docker/check-worker-providers.sh'), str(source)],
               check=True, timeout=10)
for notice in ('LICENSE', 'third_party_licenses/boost/LICENSE_1_0.txt',
               'third_party_licenses/nlohmann_json/LICENSE.MIT',
               'third_party_licenses/yaml-cpp/LICENSE',
               'share/licenses/musl-COPYRIGHT', 'share/licenses/OpenSSL-LICENSE.txt',
               'share/licenses/GCC-COPYING3', 'share/licenses/GCC-COPYING.RUNTIME',
               'share/licenses/Mozilla-MPL-2.0.txt', 'share/licenses/README.md'):
    assert (source / notice).stat().st_size > 0, notice

with tempfile.TemporaryDirectory(prefix='simplex musl ') as temporary:
    base = Path(temporary)
    original = base / 'original'
    shutil.copytree(source, original, symlinks=True)
    installed = base / 'moved installation 中文'
    original.rename(installed)
    caller = base / 'unrelated working directory'
    caller.mkdir()
    links = base / 'links'
    links.mkdir()
    (links / 'worker-next').symlink_to(installed / 'bin/simplex_worker')
    (links / 'simplex_worker').symlink_to('worker-next')
    (links / 'simplex').symlink_to(installed / 'bin/simplex')
    env = dict(os.environ, PATH=str(links) + os.pathsep + os.environ['PATH'])
    env.pop('LD_LIBRARY_PATH', None)

    for command in ([str(installed / 'bin/simplex_worker')], ['simplex_worker'],
                    [str(installed / 'bin/simplex')], ['simplex']):
        result = subprocess.run([*command, '--version'], cwd=caller, env=env,
                                text=True, capture_output=True, timeout=10)
        assert result.returncode == 0, result.stderr
        assert result.stdout == version + '\n', result.stdout

    # Production fixtures exercise provider loading, installed prompts and
    # graceful stop through the moved package, not a build-tree executable.
    for entry in ('simplex_worker', 'simplex'):
        args = ['run'] if entry == 'simplex' else []
        subprocess.run([sys.executable, str(repo / 'core/test/test_worker_cli.py'),
                        str(installed / 'bin' / entry), *args], cwd=caller,
                       env=env, check=True, timeout=60)
    subprocess.run([sys.executable, str(repo / 'core/test/test_lifecycle_container.py'),
                    str(installed / 'bin/simplex_worker')], cwd=caller, env=env,
                   check=True, timeout=90)

    # Replace just the real ELF with a probe in this throwaway copy. musl's
    # loader can launch it, making PID/cwd/argument/environment observations
    # cover the actual wrapper. Compile the probe beforehand in the build image.
    probe = Path(sys.argv[3]).resolve()
    assert probe.is_file(), probe
    shutil.copy2(probe, installed / 'bin/simplex_worker.bin')
    import json
    arguments = ['', 'space argument', '$(not executed)', 'line\none', '中文']
    explicit = dict(env, SSL_CERT_FILE='/explicit/ca.pem',
                    SSL_CERT_DIR='/explicit/certs', OPENSSL_CONF='/explicit/config')
    process = subprocess.Popen(['simplex_worker', *arguments], cwd=caller,
                               env=explicit, text=True, stdout=subprocess.PIPE)
    stdout, _ = process.communicate(timeout=10)
    data = json.loads(stdout)
    assert process.returncode == 37
    assert data['arguments'] == arguments
    assert data['pid'] == process.pid
    assert data['cwd'] == str(caller)
    assert data['library_path'] is None
    assert data['cert_file'] == '/explicit/ca.pem'
    assert data['cert_dir'] == '/explicit/certs'
    assert data['openssl_conf'] == '/explicit/config'

    # Default trust selection and caller-provided directory-only trust are
    # observable before OpenSSL startup, without any live network dependency.
    defaults = {key: value for key, value in env.items()
                if key not in ('SSL_CERT_FILE', 'SSL_CERT_DIR', 'OPENSSL_CONF')}
    result = subprocess.run(['simplex_worker'], cwd=caller, env=defaults,
                            text=True, capture_output=True, timeout=10)
    assert result.returncode == 37
    observed = json.loads(result.stdout)
    candidates = [Path('/etc/ssl/certs/ca-certificates.crt'),
                  Path('/etc/pki/tls/certs/ca-bundle.crt'), Path('/etc/ssl/cert.pem'),
                  installed / 'share/certs/ca-certificates.crt']
    expected_ca = next(path for path in candidates if os.access(path, os.R_OK))
    assert observed['cert_file'] == str(expected_ca)
    assert observed['openssl_conf'] == str(installed / 'share/openssl/openssl.cnf')
    directory_only = dict(defaults, SSL_CERT_DIR='/explicit/certs', OPENSSL_CONF='')
    result = subprocess.run(['simplex_worker'], cwd=caller, env=directory_only,
                            text=True, capture_output=True, timeout=10)
    assert result.returncode == 37
    observed = json.loads(result.stdout)
    assert observed['cert_file'] is None
    assert observed['cert_dir'] == '/explicit/certs'
    assert observed['openssl_conf'] == ''

    (installed / 'lib/ld-musl-x86_64.so.1').unlink()
    missing = subprocess.run(['simplex_worker', '--version'], cwd=caller,
                             env=env, capture_output=True, text=True, timeout=10)
    assert missing.returncode == 127
    assert 'bundled loader' in missing.stderr
print('musl relocation, symlinks, worker lifecycle and missing-loader checks passed')
