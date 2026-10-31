"""Launcher settings: what to run and how (`settings.toml`) and API keys (`secrets.toml`, mode 600).

Both files live in ~/.config/gothic-ai/ next to the ChatGPT tokens. The services do not read them: the
launcher hands the values over as flags and environment variables the services already understand (see
commands.py). Titles and descriptions are for the launcher window, which builds its form from the JSON
Schema of these models.
"""

from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal

import tomli_w
from pydantic import BaseModel, ConfigDict, Field, ValidationError

CONFIG_DIR = Path.home() / ".config" / "gothic-ai"
SETTINGS_FILE = "settings.toml"
SECRETS_FILE = "secrets.toml"

# variables of the repository's `.env` (see .env.example) and the settings they become
ENV_KEYS = {
    "GOTHIC_DIR": "game.dir",
    "GOTHIC_AI_CHAT_SCALE": "game.chat_scale",
    "GOTHIC_AI_LLM": "llm.backend",
    "GOTHIC_DECIDER_URL": "llm.decider_url",
    "GOTHIC_QUICK_LLM_URL": "llm.quick_url",
    "GOTHIC_BODY_URL": "llm.body_url",
    "GOTHIC_BODY_POLICY": "llm.body_policy",
    "OPENGOTHIC_BODY_NET": "llm.body_net",
    "GOTHIC_ASR_PROVIDER": "voice.provider",
    "GOTHIC_ASR_FALLBACK": "voice.fallback",
    "GOTHIC_VOXTRAL_URL": "voice.voxtral_url",
    "GOTHIC_TTS_PROVIDER": "tts.provider",
    "GOTHIC_TTS_FALLBACK": "tts.fallback",
    "GOTHIC_VOXCPM_URL": "tts.voxcpm_url",
    "OPENAI_API_KEY": "secrets.openai_api_key",
    "INWORLD_API_KEY": "secrets.inworld_api_key",
}


class SettingsError(Exception):
    """A settings file that cannot be read, or a value that does not fit."""


class _Section(BaseModel):
    model_config = ConfigDict(extra="forbid")


class GameSettings(_Section):
    dir: Path | None = Field(
        None, title="Папка игры",
        description="Каталог настроек с готовой базой; для первого импорта — установленная Gothic II",
    )
    dev_mode: bool = Field(
        False, title="Режим разработчика", description="Консоль Marvin (F2) и отладочные команды ИИ"
    )
    chat_scale: float | None = Field(None, gt=0, title="Масштаб окна чата", description="Пусто: 1")


class LlmSettings(_Section):
    backend: Literal["chatgpt", "openai", "local", "fake"] = Field(
        "chatgpt",
        title="Мозг NPC",
        description="chatgpt: подписка ChatGPT; openai: ключ API; local: OpenAI-совместимый сервер",
    )
    model: str | None = Field(None, title="Модель", description="Пусто: модель бэкенда по умолчанию")
    base_url: str | None = Field(None, title="Адрес API", description="Для бэкендов openai и local")
    reasoning_effort: str = Field("low", title="Глубина рассуждений")
    decider_url: str | None = Field(
        None,
        title="Адрес быстрой модели",
        description="Strands decider (ws://…): первый звук, жесты, тело, распорядок NPC. Пусто: выключена",
    )
    quick: bool = Field(
        True,
        title="Реплики в бою от Gemma",
        description="Быстрая модель на GPU-сервере: NPC в бою отвечают сразу",
    )
    quick_url: str | None = Field(
        None,
        title="Адрес модели реплик в бою",
        description="Gemma на llama.cpp (http://…/v1). Пусто: сервер decider, порт 47834",
    )
    body_policy: Literal["engine", "service"] = Field(
        "engine",
        title="Политика боя",
        description="engine: сеть P30 в движке; service: внешний TCP-сервис",
    )
    body_net: Path | None = Field(
        None,
        title="Файл сети боя",
        description="Файл .bodynet. Пусто: ~/.local/share/gothic-ai/body/nets/p30.bodynet",
    )
    body_url: str | None = Field(
        None,
        title="Адрес запасного сервиса боя",
        description=(
            "Сеть тела P30 (tcp://…:47835): для service или когда сеть движка не загружена. "
            "Пусто: доступна только сеть в движке"
        ),
    )


class VoiceSettings(_Section):
    enabled: bool = Field(True, title="Голосовой ввод", description="Удерживайте T, чтобы сказать реплику")
    provider: str = Field(
        "inworld", title="Распознавание", description="inworld, voxtral, chatgpt или openai:…"
    )
    fallback: str = Field("chatgpt", title="Запасное распознавание", description="none: без запасного")
    voxtral_url: str | None = Field(None, title="Адрес Voxtral")
    mic_device: str | None = Field(None, title="Микрофон", description="Имя или номер; пусто: системный")


class TtsSettings(_Section):
    enabled: bool = Field(True, title="Озвучка NPC", description="Выключено: только субтитры")
    original_voices: bool = Field(
        True, title="Оригинальные голоса", description="Клоны оригинальных голосов сюжетных NPC в Inworld"
    )
    provider: str = Field("inworld", title="Голоса", description="inworld, voxcpm или openai:…")
    fallback: str = Field("none", title="Запасные голоса", description="none: без запасного")
    voxcpm_url: str | None = Field(None, title="Адрес VoxCPM2")
    loudness: float | Literal["none"] | None = Field(
        None,
        title="Громкость реплик, LUFS",
        description="Пусто: как озвучка игры; none: как отдал провайдер",
    )


class PathSettings(_Section):
    repo: Path | None = Field(None, title="Репозиторий", description="Пусто: тот, где лежит лаунчер")
    uv: Path | None = Field(None, title="Путь к uv", description="Пусто: найти самому")
    game_bin: Path | None = Field(
        None, title="Исполняемый файл игры", description="Пусто: OpenGothic/build/opengothic/Gothic2Notr"
    )
    game_home: Path | None = Field(
        None,
        title="Папка сохранений",
        description="Туда игра пишет сохранения, Gothic.ini и log.txt. Пусто: ~/.local/share/gothic-ai/game",
    )


class Settings(_Section):
    game: GameSettings = Field(default_factory=GameSettings, title="Игра")
    llm: LlmSettings = Field(default_factory=LlmSettings, title="Мозг NPC")
    voice: VoiceSettings = Field(default_factory=VoiceSettings, title="Голос игрока")
    tts: TtsSettings = Field(default_factory=TtsSettings, title="Озвучка NPC")
    paths: PathSettings = Field(default_factory=PathSettings, title="Дополнительно")


class Secrets(_Section):
    openai_api_key: str | None = Field(None, title="Ключ OpenAI", json_schema_extra={"secret": True})
    inworld_api_key: str | None = Field(None, title="Ключ Inworld", json_schema_extra={"secret": True})


@dataclass
class Config:
    settings: Settings
    secrets: Secrets
    dir: Path

    @property
    def settings_path(self) -> Path:
        return self.dir / SETTINGS_FILE

    @property
    def secrets_path(self) -> Path:
        return self.dir / SECRETS_FILE

    @classmethod
    def load(cls, config_dir: Path = CONFIG_DIR) -> Config:
        """Missing files mean defaults."""
        settings = _load(Settings, config_dir / SETTINGS_FILE)
        return cls(settings, _load(Secrets, config_dir / SECRETS_FILE), config_dir)

    def save(self) -> None:
        self.dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        header = "# Gothic AI launcher settings (`gothic-launcher config`). API keys are in secrets.toml.\n\n"
        _write(self.settings_path, header + tomli_w.dumps(_plain(self.settings)), 0o644)
        _write(self.secrets_path, tomli_w.dumps(_plain(self.secrets)), 0o600)

    def get(self, key: str) -> Any:
        model, field = self._locate(key)
        return getattr(model, field)

    def set(self, key: str, value: Any) -> None:
        """`value` None goes back to the default. Strings are converted to the field's type."""
        model, field = self._locate(key)
        data = model.model_dump(exclude_none=True)
        if value is None:
            data.pop(field, None)
        else:
            data[field] = value
        try:
            updated = type(model).model_validate(data)
        except ValidationError as err:
            raise SettingsError(f"{key}: {err.errors()[0]['msg']}") from None
        section = key.partition(".")[0]
        if section == "secrets":
            self.secrets = updated  # type: ignore[assignment]
        else:
            setattr(self.settings, section, updated)

    def keys(self) -> list[str]:
        sections: dict[str, BaseModel] = {n: getattr(self.settings, n) for n in Settings.model_fields}
        sections["secrets"] = self.secrets
        return [f"{name}.{field}" for name, model in sections.items() for field in type(model).model_fields]

    def is_secret(self, key: str) -> bool:
        return key.startswith("secrets.")

    def show(self) -> str:
        """Both files as TOML; keys only say whether they are set."""
        secrets = {k: "<set>" if v else "<not set>" for k, v in self.secrets.model_dump().items()}
        return tomli_w.dumps(_plain(self.settings)) + "\n" + tomli_w.dumps({"secrets": secrets})

    def import_env(self, path: Path) -> tuple[list[str], list[str]]:
        """Takes the values a `.env` holds (see ENV_KEYS). Returns the settings it set and the variables it
        did not know."""
        imported: list[str] = []
        skipped: list[str] = []
        for name, value in read_env_file(path).items():
            key = ENV_KEYS.get(name)
            if key is None:
                skipped.append(name)
            elif value:
                self.set(key, value)
                imported.append(key)
        return imported, skipped

    def _locate(self, key: str) -> tuple[BaseModel, str]:
        section, _, field = key.partition(".")
        model: BaseModel | None
        if section == "secrets":
            model = self.secrets
        else:
            model = getattr(self.settings, section, None) if section in Settings.model_fields else None
        if model is None or field not in type(model).model_fields:
            raise SettingsError(f"unknown setting {key!r}; known: {', '.join(self.keys())}")
        return model, field


def read_env_file(path: Path) -> dict[str, str]:
    """`KEY=value` lines as the services read them (cli.load_env_file)."""
    out: dict[str, str] = {}
    for line in path.read_text().splitlines():
        key, sep, value = line.strip().removeprefix("export ").partition("=")
        if sep and key and not key.startswith("#"):
            out[key.strip()] = value.strip().strip("'\"")
    return out


def _load[M: BaseModel](model: type[M], path: Path) -> M:
    if not path.is_file():
        return model()
    try:
        return model.model_validate(tomllib.loads(path.read_text()))
    except tomllib.TOMLDecodeError as err:
        raise SettingsError(f"{path}: {err}") from None
    except ValidationError as err:
        e = err.errors()[0]
        raise SettingsError(f"{path}: {'.'.join(map(str, e['loc']))}: {e['msg']}") from None


def _plain(model: BaseModel) -> dict[str, Any]:
    return model.model_dump(mode="json", exclude_none=True)


def _write(path: Path, text: str, mode: int) -> None:
    """Atomic, and the file never exists with looser permissions than `mode`."""
    tmp = path.with_name(path.name + ".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    try:
        os.fchmod(fd, mode)
        os.write(fd, text.encode())
    finally:
        os.close(fd)
    os.replace(tmp, path)
