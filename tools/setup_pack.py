"""下载并校验普冉官方芯片包，供 pyOCD 识别目标和加载 Flash 算法。"""
import hashlib
import io
from pathlib import Path
import urllib.request
import urllib.parse
import zipfile

URL = "https://www.puyasemi.com/download_path/工具/MCU 微处理器/PY32F0xx_Keil_DFP_V1.2.20.zip"
NAME = "Puya.PY32F0xx_DFP.1.2.20.pack"
SHA256 = "bf3ddd933f28f4557c90a8070cbc834bd3869ff88e53a8b20d80e79c51f842c4"


def main():
    # 已有文件校验通过即可复用，否则重新从官方 ZIP 中读取指定版本。
    dest = Path(__file__).resolve().parent.parent / "packs" / NAME
    if dest.is_file() and hashlib.sha256(dest.read_bytes()).hexdigest() == SHA256:
        print("Device pack is ready:", dest)
        return
    print("Downloading the official Puya DFP (requires internet)...")
    url = urllib.parse.quote(URL, safe=":/%")
    with urllib.request.urlopen(url, timeout=60) as response:
        archive = response.read()
    with zipfile.ZipFile(io.BytesIO(archive)) as package:
        data = package.read(NAME)
    # 仅保存通过 SHA-256 校验的芯片包。
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise RuntimeError("Device pack checksum mismatch; download was not saved.")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    print("Device pack is ready:", dest)


if __name__ == "__main__":
    main()
