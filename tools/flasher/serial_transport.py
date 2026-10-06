"""Serial transport for MCUboot firmware updates via mcumgr CLI."""

import os
import queue
import re
import subprocess
import sys
import threading
import time


class SerialTransport:
    """Wraps mcumgr CLI for serial-based firmware updates."""

    def __init__(self, mcumgr_path, port, baudrate='921600'):
        self.mcumgr_path = str(mcumgr_path)
        self.port = port
        self.baudrate = baudrate
        self.current_process = None
        self._cancelled = False

    def cancel(self):
        self._cancelled = True
        if self.current_process:
            self.current_process.terminate()
            try:
                self.current_process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.current_process.kill()
                self.current_process.wait(timeout=2)

    @staticmethod
    def _stop(process):
        """Stop mcumgr for good: a process left alive keeps the port open
        and garbles the next command."""
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

    def run(self, args, timeout=0, on_line=None):
        """Execute mcumgr command.

        Args:
            args: Command arguments for mcumgr.
            timeout: Max seconds to wait (0 = no timeout).
            on_line: Callback(line, is_progress) for streaming output.

        Returns:
            (success, stdout, stderr)
        """
        cmd = [
            self.mcumgr_path,
            "--conntype", "serial",
            "--connstring", f"{self.port},baud={self.baudrate},mtu=4096",
        ] + args

        try:
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
            )
            self.current_process = process

            stdout_data = []
            stderr_data = []
            current_line = ""
            start_time = time.monotonic()

            if sys.platform != "win32":
                return self._run_unix(process, timeout, on_line,
                                      stdout_data, stderr_data, start_time)
            else:
                return self._run_windows(process, timeout, on_line,
                                         stdout_data, stderr_data, start_time)

        except Exception as e:
            return False, "", str(e)

    def _run_unix(self, process, timeout, on_line, stdout_data, stderr_data, start_time):
        """Unix implementation using select + non-blocking I/O."""
        import select
        import fcntl

        for pipe in [process.stdout, process.stderr]:
            fd = pipe.fileno()
            fl = fcntl.fcntl(fd, fcntl.F_GETFL)
            fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)

        current_line = ""

        while process.poll() is None and not self._cancelled:
            if timeout > 0 and (time.monotonic() - start_time) > timeout:
                self._stop(process)
                return False, ''.join(stdout_data), f"Timeout after {timeout}s"

            readable, _, _ = select.select([process.stdout, process.stderr], [], [], 0.1)

            for pipe in readable:
                try:
                    data = pipe.read(1024)
                    if data:
                        text = data.decode('utf-8', errors='replace')
                        if pipe == process.stdout:
                            stdout_data.append(text)
                        else:
                            stderr_data.append(text)

                        if on_line:
                            for char in text:
                                if char == '\r':
                                    if current_line:
                                        on_line(current_line, True)
                                    current_line = ""
                                elif char == '\n':
                                    if current_line:
                                        on_line(current_line, False)
                                    current_line = ""
                                else:
                                    current_line += char
                except Exception:
                    pass

        if not self._cancelled:
            remaining_stdout, remaining_stderr = process.communicate()
            if remaining_stdout:
                text = remaining_stdout.decode('utf-8', errors='replace')
                stdout_data.append(text)
                if on_line and text.strip():
                    on_line(text.rstrip(), False)
            if remaining_stderr:
                stderr_data.append(remaining_stderr.decode('utf-8', errors='replace'))

        success = process.returncode == 0 if not self._cancelled else False
        return success, ''.join(stdout_data), ''.join(stderr_data)

    def _run_windows(self, process, timeout, on_line, stdout_data, stderr_data, start_time):
        """Windows implementation using threads for non-blocking read."""
        line_queue = queue.Queue() if on_line else None

        def read_stdout(pipe, data_list, lq):
            current_line = ""
            try:
                while True:
                    chunk = pipe.read(1024)
                    if not chunk:
                        break
                    text = chunk.decode('utf-8', errors='replace')
                    data_list.append(text)
                    if lq:
                        for char in text:
                            if char == '\r':
                                if current_line:
                                    lq.put((current_line, True))
                                current_line = ""
                            elif char == '\n':
                                if current_line:
                                    lq.put((current_line, False))
                                current_line = ""
                            else:
                                current_line += char
            except Exception:
                pass
            if current_line and lq:
                lq.put((current_line, False))

        def read_stderr(pipe, data_list):
            try:
                for line in iter(pipe.readline, b''):
                    data_list.append(line.decode('utf-8', errors='replace'))
            except Exception:
                pass

        stdout_thread = threading.Thread(target=read_stdout,
                                         args=(process.stdout, stdout_data, line_queue), daemon=True)
        stderr_thread = threading.Thread(target=read_stderr,
                                         args=(process.stderr, stderr_data), daemon=True)
        stdout_thread.start()
        stderr_thread.start()

        while process.poll() is None and not self._cancelled:
            if timeout > 0 and (time.monotonic() - start_time) > timeout:
                self._stop(process)
                return False, ''.join(stdout_data), f"Timeout after {timeout}s"

            # Drain line queue
            if line_queue:
                try:
                    while True:
                        line, is_progress = line_queue.get_nowait()
                        on_line(line, is_progress)
                except queue.Empty:
                    pass

            time.sleep(0.1)

        stdout_thread.join(timeout=2)
        stderr_thread.join(timeout=2)

        success = process.returncode == 0 if not self._cancelled else False
        return success, ''.join(stdout_data), ''.join(stderr_data)

    @staticmethod
    def parse_hash(list_output, target_version=None, image_num_filter=None):
        """Parse image list to find the hash of the new image in slot 1.

        Args:
            list_output: Output from 'mcumgr image list'
            target_version: Optional version to match
            image_num_filter: Optional image number to filter

        Returns:
            Hash string or None
        """
        image_pattern = re.compile(
            r'image=(\d+)\s+slot=(\d+)\s+'
            r'version:\s+(\S+)\s+'
            r'bootable:\s+(\w+)\s+'
            r'flags:\s*(.*?)\s*'
            r'hash:\s+([a-f0-9]+)',
            re.MULTILINE | re.DOTALL
        )

        matches = image_pattern.findall(list_output)

        for match in matches:
            image_num, slot, version, bootable, flags, hash_value = match
            flags = flags.strip()

            if slot == "1" and "active" not in flags and "confirmed" not in flags:
                if image_num_filter is not None and int(image_num) != image_num_filter:
                    continue
                if target_version is None or version == target_version:
                    return hash_value

        return None
