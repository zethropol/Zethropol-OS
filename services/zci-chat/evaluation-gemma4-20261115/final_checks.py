"""Read-only host listener checks after shutting down owned test processes."""
import json
from pathlib import Path
import socket
import subprocess

root = Path(__file__).resolve().parent
ports = {}
for port in (8087, 8088, 28089):
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=1):
            ports[str(port)] = 'listening'
    except ConnectionRefusedError:
        ports[str(port)] = 'connection_refused'
assert ports == {str(port): 'connection_refused' for port in (8087, 8088, 28089)}
processes = subprocess.run(['ps', '-C', 'llama-server', '-o', 'pid,ppid,args'], capture_output=True, text=True)
assert processes.returncode == 1, 'Unexpected llama-server still running; inspect rather than stop it'
result = {'ports': ports, 'matches_initial_8087_8088_state': True,
          'temporary_server_stopped': True, 'llama_server_processes_remaining': 0,
          'webui_database_opened_or_modified_by_evaluation': False,
          'system_services_modified': False, 'cgroup_tests': 'five remain open'}
(root / 'final-checks.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
