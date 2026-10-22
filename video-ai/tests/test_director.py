import pytest

from cena.director import ESTILOS, MOVIMENTOS, NEGATIVO, dirigir, quadros_para


def test_pedido_vem_primeiro_e_recebe_linguagem_de_cinema():
    d = dirigir("  Um homem   acorda com o despertador. ", "comercial", "aproximando")
    assert d.prompt.startswith("Um homem acorda com o despertador. ")
    assert ESTILOS["comercial"] in d.prompt
    assert MOVIMENTOS["aproximando"] in d.prompt
    assert "photorealistic" in d.prompt
    assert d.negativo == NEGATIVO


def test_movimento_auto_nao_deixa_buraco_no_prompt():
    d = dirigir("praia", "drone", "auto")
    assert ". ." not in d.prompt and ".." not in d.prompt


def test_extra_negativo_e_validacoes():
    assert dirigir("x", extra_negativo=" chapéu ").negativo.endswith(", chapéu")
    with pytest.raises(ValueError):
        dirigir("   ")
    with pytest.raises(ValueError):
        dirigir("x", estilo="pixar")
    with pytest.raises(ValueError):
        dirigir("x", movimento="voando")


@pytest.mark.parametrize("seg,fps,esperado", [(5, 24, 121), (5, 16, 81), (2, 24, 49), (0.1, 16, 5)])
def test_quadros_no_formato_4k_mais_1(seg, fps, esperado):
    q = quadros_para(seg, fps)
    assert q == esperado and (q - 1) % 4 == 0
