"""Roblox Fixer & Launcher Pro.

Aplicativo Windows com interface CustomTkinter para tarefas comuns de reparo do
Roblox e abertura direta de experiências pelo protocolo roblox-player.
"""

from __future__ import annotations

import os
import queue
import re
import subprocess
import threading
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Callable

import customtkinter as ctk
from tkinter import messagebox


APP_TITLE = "Roblox Fixer & Launcher Pro"
WINDOW_SIZE = (540, 500)
CREATE_NO_WINDOW = 0x08000000
PLACE_ID_PATTERN = re.compile(r"^[0-9]{1,20}$")


class RepairError(RuntimeError):
    """Erro esperado durante uma operação de correção."""


class EventKind(Enum):
    SUCCESS = "success"
    ERROR = "error"


@dataclass(frozen=True)
class UIEvent:
    kind: EventKind
    message: str


def _windows_only() -> None:
    if os.name != "nt":
        raise RepairError("Este recurso está disponível somente no Windows.")


def run_silent_command(command: str) -> subprocess.CompletedProcess[str]:
    """Executa um comando do Windows sem criar uma janela de console.

    ``shell=True`` é necessário para comandos internos, como ``del``. Todo valor
    variável usado pelos comandos abaixo vem do ambiente local e é colocado
    entre aspas; nenhuma entrada do usuário é interpolada no shell.
    """

    _windows_only()
    return subprocess.run(
        command,
        shell=True,
        creationflags=CREATE_NO_WINDOW,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


def _quote_cmd_path(path: Path) -> str:
    # Os destinos são construídos apenas a partir de diretórios conhecidos.
    # Aspas em variáveis de ambiente seriam perigosas em cmd.exe.
    value = str(path)
    if '"' in value or "\r" in value or "\n" in value:
        raise RepairError("Um caminho do sistema contém caracteres inválidos.")
    return f'"{value}"'


def _clear_directory_contents(path: Path) -> None:
    quoted_path = _quote_cmd_path(path)
    quoted_children = _quote_cmd_path(path / "*")
    command = (
        f"if exist {quoted_path} ("
        f"del /f /s /q {quoted_children} >nul 2>&1 & "
        f'for /d %D in ({quoted_children}) do @rd /s /q "%D" >nul 2>&1'
        ") & exit /b 0"
    )
    result = run_silent_command(command)
    if result.returncode != 0:
        raise RepairError(f"Não foi possível limpar: {path}")


def clear_cache_and_temporary_files() -> str:
    """Remove somente caches, downloads temporários e logs do Roblox."""

    _windows_only()
    local_app_data = os.environ.get("LOCALAPPDATA")
    temp = os.environ.get("TEMP")
    if not local_app_data:
        raise RepairError("A variável LOCALAPPDATA não foi encontrada.")

    roblox = Path(local_app_data) / "Roblox"
    targets = [
        roblox / "Downloads",
        roblox / "logs",
        roblox / "http",
        roblox / "LocalStorage" / "roblox",
    ]
    if temp:
        targets.append(Path(temp) / "Roblox")

    for target in targets:
        _clear_directory_contents(target)

    return "Cache e arquivos temporários foram limpos."


def reset_graphics_settings() -> str:
    """Exclui o XML gráfico; o Roblox o recriará no próximo início."""

    _windows_only()
    local_app_data = os.environ.get("LOCALAPPDATA")
    if not local_app_data:
        raise RepairError("A variável LOCALAPPDATA não foi encontrada.")

    settings_file = Path(local_app_data) / "Roblox" / "GlobalBasicSettings_13.xml"
    quoted_file = _quote_cmd_path(settings_file)
    result = run_silent_command(
        f"if exist {quoted_file} del /f /q {quoted_file} >nul 2>&1 & exit /b 0"
    )
    if result.returncode != 0:
        raise RepairError(
            "Não foi possível redefinir o XML. Feche o Roblox e tente novamente."
        )
    return "As configurações gráficas foram redefinidas."


def optimize_network() -> str:
    """Limpa o DNS e redefine o catálogo Winsock do Windows."""

    _windows_only()
    commands = (
        ("ipconfig /flushdns", "limpar o cache DNS"),
        ("netsh winsock reset", "redefinir o Winsock"),
    )
    for command, description in commands:
        result = run_silent_command(command)
        if result.returncode != 0:
            details = (result.stderr or result.stdout).strip()
            suffix = f"\n\nDetalhes: {details}" if details else ""
            raise RepairError(
                f"Não foi possível {description}. Execute o aplicativo como "
                f"administrador e tente novamente.{suffix}"
            )
    return "DNS limpo e Winsock redefinido. Reinicie o computador para concluir."


def launch_place(place_id: str) -> None:
    """Abre um ID numérico no protocolo registrado pelo Roblox Player."""

    _windows_only()
    normalized = place_id.strip()
    if not PLACE_ID_PATTERN.fullmatch(normalized):
        raise RepairError("Digite um ID válido contendo apenas números.")

    try:
        os.startfile(  # type: ignore[attr-defined]
            f"roblox-player:1+launchmode:play+gameinfo:{normalized}"
        )
    except OSError as exc:
        raise RepairError(
            "Não foi possível abrir o Roblox Player. Confirme se ele está instalado."
        ) from exc


class RobloxFixerApp(ctk.CTk):
    """Janela principal do aplicativo."""

    def __init__(self) -> None:
        super().__init__()
        self.title(APP_TITLE)
        self.geometry(f"{WINDOW_SIZE[0]}x{WINDOW_SIZE[1]}")
        self.resizable(False, False)
        self._center_window()

        self._events: queue.Queue[UIEvent] = queue.Queue()
        self._repair_buttons: list[ctk.CTkButton] = []

        self._configure_theme()
        self._build_interface()
        self.after(100, self._process_events)

    def _configure_theme(self) -> None:
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(1, weight=1)

    def _center_window(self) -> None:
        self.update_idletasks()
        width, height = WINDOW_SIZE
        x = max(0, (self.winfo_screenwidth() - width) // 2)
        y = max(0, (self.winfo_screenheight() - height) // 2)
        self.geometry(f"{width}x{height}+{x}+{y}")

    def _build_interface(self) -> None:
        header = ctk.CTkFrame(self, fg_color="transparent")
        header.grid(row=0, column=0, padx=28, pady=(20, 10), sticky="ew")
        ctk.CTkLabel(
            header,
            text=APP_TITLE,
            font=ctk.CTkFont(size=24, weight="bold"),
        ).pack()
        ctk.CTkLabel(
            header,
            text="Correções rápidas e acesso direto ao Roblox",
            text_color=("gray38", "gray68"),
            font=ctk.CTkFont(size=13),
        ).pack(pady=(3, 0))

        tabs = ctk.CTkTabview(self, corner_radius=14)
        tabs.grid(row=1, column=0, padx=28, pady=(0, 14), sticky="nsew")
        fixes_tab = tabs.add("Correções")
        launcher_tab = tabs.add("Launcher Direto")
        self._build_fixes_tab(fixes_tab)
        self._build_launcher_tab(launcher_tab)

        self.status_label = ctk.CTkLabel(
            self,
            text="Pronto",
            anchor="w",
            text_color=("gray35", "gray70"),
        )
        self.status_label.grid(row=2, column=0, padx=32, pady=(0, 4), sticky="ew")
        self.progress = ctk.CTkProgressBar(self, height=7, mode="indeterminate")
        self.progress.grid(row=3, column=0, padx=32, pady=(0, 20), sticky="ew")
        self.progress.set(0)

    def _build_fixes_tab(self, parent: ctk.CTkFrame) -> None:
        parent.grid_columnconfigure(0, weight=1)
        actions: tuple[tuple[str, Callable[[], str]], ...] = (
            ("Limpar Cache e Temporários", clear_cache_and_temporary_files),
            ("Redefinir Configurações Gráficas (XML)", reset_graphics_settings),
            ("Otimizar Rede (DNS/Winsock)", optimize_network),
        )
        for row, (label, action) in enumerate(actions):
            button = ctk.CTkButton(
                parent,
                text=label,
                height=56,
                corner_radius=10,
                font=ctk.CTkFont(size=14, weight="bold"),
                command=lambda task=action: self._start_repair(task),
            )
            button.grid(row=row, column=0, padx=18, pady=(14 if row == 0 else 8, 5), sticky="ew")
            self._repair_buttons.append(button)

        ctk.CTkLabel(
            parent,
            text="Dica: a otimização de rede requer permissão de administrador.",
            text_color=("gray42", "gray62"),
            font=ctk.CTkFont(size=11),
        ).grid(row=3, column=0, padx=18, pady=(8, 0))

    def _build_launcher_tab(self, parent: ctk.CTkFrame) -> None:
        parent.grid_columnconfigure(0, weight=1)
        ctk.CTkLabel(
            parent,
            text="Abra uma experiência sem passar pelo navegador",
            font=ctk.CTkFont(size=15, weight="bold"),
        ).grid(row=0, column=0, padx=22, pady=(32, 10))

        self.place_id_entry = ctk.CTkEntry(
            parent,
            placeholder_text="Cole o ID do Jogo/Mapa aqui",
            height=48,
            corner_radius=10,
            justify="center",
            font=ctk.CTkFont(size=14),
        )
        self.place_id_entry.grid(row=1, column=0, padx=30, pady=10, sticky="ew")
        self.place_id_entry.bind("<Return>", lambda _event: self._launch_game())

        ctk.CTkButton(
            parent,
            text="Iniciar Jogo (Ignorar Navegador)",
            height=54,
            corner_radius=10,
            fg_color=("#16A34A", "#22C55E"),
            hover_color=("#15803D", "#16A34A"),
            text_color=("white", "#07140B"),
            font=ctk.CTkFont(size=14, weight="bold"),
            command=self._launch_game,
        ).grid(row=2, column=0, padx=30, pady=(12, 8), sticky="ew")

    def _set_busy(self, busy: bool) -> None:
        state = "disabled" if busy else "normal"
        for button in self._repair_buttons:
            button.configure(state=state)
        if busy:
            self.progress.start()
        else:
            self.progress.stop()
            self.progress.set(1)

    def _start_repair(self, task: Callable[[], str]) -> None:
        self._set_busy(True)
        self.status_label.configure(text="Corrigindo...", text_color=("#B45309", "#FBBF24"))

        def worker() -> None:
            try:
                message = task()
            except Exception as exc:  # A thread nunca deve encerrar silenciosamente.
                self._events.put(UIEvent(EventKind.ERROR, str(exc)))
            else:
                self._events.put(UIEvent(EventKind.SUCCESS, message))

        threading.Thread(target=worker, daemon=True, name="roblox-repair").start()

    def _process_events(self) -> None:
        try:
            while True:
                event = self._events.get_nowait()
                self._set_busy(False)
                if event.kind is EventKind.SUCCESS:
                    self.status_label.configure(
                        text="Sucesso!", text_color=("#15803D", "#4ADE80")
                    )
                    messagebox.showinfo(APP_TITLE, event.message, parent=self)
                elif event.kind is EventKind.ERROR:
                    self.status_label.configure(
                        text="Falha na correção", text_color=("#B91C1C", "#F87171")
                    )
                    messagebox.showerror(APP_TITLE, event.message, parent=self)
        except queue.Empty:
            pass
        finally:
            self.after(100, self._process_events)

    def _launch_game(self) -> None:
        try:
            launch_place(self.place_id_entry.get())
        except RepairError as exc:
            messagebox.showwarning(APP_TITLE, str(exc), parent=self)
            return
        self.status_label.configure(text="Roblox iniciado!", text_color=("#15803D", "#4ADE80"))


def main() -> None:
    ctk.set_appearance_mode("system")
    ctk.set_default_color_theme("blue")
    app = RobloxFixerApp()
    app.mainloop()


if __name__ == "__main__":
    main()
