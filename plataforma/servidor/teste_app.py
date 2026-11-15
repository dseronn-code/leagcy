"""Testes do servidor de contas (python3 -m pytest teste_app.py, de plataforma/servidor)."""
import os
import re
import tempfile

os.environ['LEGACY_DB'] = os.path.join(tempfile.mkdtemp(), 'teste.db')

from fastapi.testclient import TestClient  # noqa: E402

import app as servidor  # noqa: E402

c = TestClient(servidor.app)
enviados = []
servidor.enviar_email = lambda para, assunto, texto: enviados.append((para, texto))


def codigo():
    return re.search(r'\b(\d{6})\b', enviados[-1][1]).group(1)


def test_fluxo_completo():
    r = c.post('/conta/cadastro', json={'email': 'Ana@Exemplo.com', 'senha': 'segredo123'})
    assert r.status_code == 200
    assert c.post('/sessao/login', json={'email': 'ana@exemplo.com', 'senha': 'segredo123'}).status_code == 403
    assert c.post('/conta/verificar', json={'email': 'ana@exemplo.com', 'codigo': '000000' if codigo() != '000000' else '111111'}).status_code == 400
    r = c.post('/conta/verificar', json={'email': 'ana@exemplo.com', 'codigo': codigo(), 'app': 'launcher'})
    assert r.status_code == 200, r.text
    tok = r.json()['token']
    assert r.json()['perfil']['precisa_username']
    h = {'Authorization': 'Bearer ' + tok}
    assert c.post('/conta/username', json={'username': 'a'}, headers=h).status_code == 400
    r = c.post('/conta/username', json={'username': 'AnaPlays'}, headers=h)
    assert r.json()['username'] == 'AnaPlays'
    # a mesma conta no Legacy Engine: login com outro app, mesmo perfil e mesmas configurações
    r = c.post('/sessao/login', json={'email': 'ana@exemplo.com', 'senha': 'segredo123', 'app': 'legacy-engine'})
    h2 = {'Authorization': 'Bearer ' + r.json()['token']}
    c.put('/eu/configuracoes', json={'idioma': 'pt', 'qualidade': 'alto'}, headers=h)
    assert c.get('/eu/configuracoes', headers=h2).json() == {'idioma': 'pt', 'qualidade': 'alto'}
    assert c.get('/eu', headers=h2).json()['username'] == 'AnaPlays'
    assert c.post('/eu/biblioteca/times-square', headers=h2).status_code == 200
    assert c.get('/eu/biblioteca', headers=h).json()[0]['produto'] == 'times-square'
    assert c.post('/sessao/login', json={'email': 'ana@exemplo.com', 'senha': 'errada123'}).status_code == 401


def test_username_unico_e_visitante():
    c.post('/conta/cadastro', json={'email': 'bia@exemplo.com', 'senha': 'outrasenha1'})
    tok = c.post('/conta/verificar', json={'email': 'bia@exemplo.com', 'codigo': codigo()}).json()['token']
    r = c.post('/conta/username', json={'username': 'anaplays'}, headers={'Authorization': 'Bearer ' + tok})
    assert r.status_code == 409  # sem diferença de maiúsculas
    v = c.post('/sessao/visitante').json()['token']
    hv = {'Authorization': 'Bearer ' + v}
    assert c.get('/eu', headers=hv).json() == {'visitante': True}
    assert c.get('/eu/configuracoes', headers=hv).status_code == 403
    assert c.get('/catalogo').json()['produtos'][0]['id'] == 'times-square'


def test_sair_e_apagar():
    c.post('/conta/cadastro', json={'email': 'caio@exemplo.com', 'senha': 'caio12345'})
    tok = c.post('/conta/verificar', json={'email': 'caio@exemplo.com', 'codigo': codigo()}).json()['token']
    h = {'Authorization': 'Bearer ' + tok}
    assert c.delete('/conta', headers=h).status_code == 200
    assert c.get('/eu', headers=h).status_code == 401
