import json
import shutil
import subprocess
from types import SimpleNamespace

import numpy as np
import pytest

from cena.director import dirigir
from cena.engine import MODELOS, Motor, Pedido, modo_recomendado


class PipeFalso:
    """Imita o WanPipeline: devolve quadros 0..1 e chama o callback a cada passo."""

    def __init__(self):
        self.chamadas = []

    def __call__(self, **kw):
        self.chamadas.append(kw)
        for i in range(kw["num_inference_steps"]):
            kw["callback_on_step_end"](self, i, None, {})
        h, w, n = kw["height"], kw["width"], kw["num_frames"]
        grad = np.linspace(0, 1, w, dtype=np.float32)[None, :, None].repeat(h, 0).repeat(3, 2)
        return SimpleNamespace(frames=[[grad * (k / n) for k in range(n)]])


class MotorFalso(Motor):
    def _executar(self, pipe, *, semente, progresso, total, **kw):
        def cb(_p, passo, _t, ckw):
            if progresso:
                progresso(passo + 1, total)
            return ckw

        return pipe(callback_on_step_end=cb, **kw).frames[0]


def test_gera_mp4_com_parametros_do_modelo(tmp_path):
    carregados = []
    pipe = PipeFalso()
    motor = MotorFalso(tmp_path, carregar=lambda m: (carregados.append(m.repo), pipe)[1])
    passos = []
    r = motor.gerar(Pedido(dirigir("teste"), modo="rapida", segundos=2, semente=7, passos=3), progresso=lambda p, t: passos.append((p, t)))
    kw = pipe.chamadas[0]
    assert (kw["width"], kw["height"]) == MODELOS["rapida"].resolucoes["480p"]
    assert kw["num_frames"] == 33 and r.quadros == 33 and r.semente == 7
    assert passos == [(1, 3), (2, 3), (3, 3)]
    assert r.arquivo.exists() and r.arquivo.stat().st_size > 1000
    # trocar de modo recarrega; repetir o modo reaproveita
    motor.gerar(Pedido(dirigir("teste"), modo="rapida", segundos=2, passos=1))
    assert carregados == [MODELOS["rapida"].repo]


def test_vertical_e_fallback_de_resolucao(tmp_path):
    pipe = PipeFalso()
    motor = MotorFalso(tmp_path, carregar=lambda m: pipe)
    motor.gerar(Pedido(dirigir("x"), modo="rapida", resolucao="720p", segundos=1, passos=1, vertical=True))
    assert (pipe.chamadas[0]["width"], pipe.chamadas[0]["height"]) == (480, 832)


@pytest.mark.skipif(not shutil.which("ffprobe"), reason="ffprobe ausente")
def test_mp4_e_h264_tocavel(tmp_path):
    motor = MotorFalso(tmp_path, carregar=lambda m: PipeFalso())
    r = motor.gerar(Pedido(dirigir("x"), modo="maxima", resolucao="480p", segundos=2, passos=1))
    info = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams", "-of", "json", str(r.arquivo)]))
    s = info["streams"][0]
    assert s["codec_name"] == "h264" and s["pix_fmt"] == "yuv420p" and int(s["nb_frames"]) == 49


def test_modo_recomendado_pela_vram():
    assert modo_recomendado(15.9) == "maxima"  # T4 do Kaggle/Colab
    assert modo_recomendado(8) == "rapida"


def test_interface_monta_sem_gpu(tmp_path):
    gr = pytest.importorskip("gradio")
    from cena.app import criar_app

    demo = criar_app(MotorFalso(tmp_path, carregar=lambda m: PipeFalso()))
    assert isinstance(demo, gr.Blocks)
