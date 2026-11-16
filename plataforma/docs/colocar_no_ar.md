# Colocar a conta Legacy no ar (grátis, sem cartão)

O launcher, o Legacy Engine e os jogos falam com o servidor de contas em `https://account.legacyengine.com.br`.
Enquanto ele não estiver no ar, o launcher mostra "Sem conexão" (e deixa jogar offline o que já estiver instalado).
São 3 serviços gratuitos, todos com login pelo GitHub e **sem cartão de crédito**:

| Serviço | Para quê | Plano grátis |
|---|---|---|
| **Neon** (neon.tech) | banco de dados (Postgres) | 0,5 GB, sempre ligado |
| **Resend** (resend.com) | e-mail com o código | 3.000 e-mails/mês (100/dia) |
| **Render** (render.com) | o servidor | dorme depois de 15 min sem uso; acorda em ~1 min |

Ordem: Neon → Resend → Render → DNS. Uns 20 minutos no total.

## 1. Neon (banco)
1. Entre em neon.tech → **Sign up** → **Continue with GitHub**.
2. Crie o projeto: nome `legacy`, Postgres 16, região **AWS US East 2 (Ohio)** (perto do servidor).
3. Na tela do projeto, **Connect** → copie a **connection string** (começa com `postgresql://`).
   **Não mande para ninguém** (nem para o Claude): ela vai direto no Render (passo 3).

## 2. Resend (e-mail)
1. Entre em resend.com → **Sign up** (com GitHub).
2. **Domains** → **Add Domain** → `legacyengine.com.br` → região **São Paulo (sa-east-1)**.
3. Ele mostra uns 3 ou 4 registros DNS (tipo **MX** e **TXT**). Coloque cada um no painel onde fica o DNS do
   domínio (passo 4) e volte aqui em **Verify**. Pode levar de minutos a algumas horas.
4. **API Keys** → **Create API Key** → permissão **Sending access** → copie a chave (começa com `re_`). Também vai
   direto no Render.

**Alternativa sem o Resend:** mandar pela caixa `equipe@legacyengine.com.br` da Hostinger. No Render, em
**Environment**, troque `LEGACY_EMAIL` para `smtp` e crie `LEGACY_SMTP_USUARIO` = `equipe@legacyengine.com.br` e
`LEGACY_SMTP_SENHA` = a senha dessa caixa (servidor `smtp.hostinger.com`, porta 465, já são o padrão). Atenção: o
plano grátis do Render pode bloquear o envio por SMTP; se os e-mails não chegarem, use o Resend.

## 3. Render (servidor)
1. Entre em render.com → **Get Started** → **GitHub**.
2. **New** → **Blueprint** → escolha o repositório `seronzera/leagcy` → ele lê o `render.yaml` sozinho.
3. Ele pede 2 valores: `DATABASE_URL` (a connection string do Neon) e `RESEND_API_KEY` (a chave do Resend).
   Cole e clique em **Apply**. (O segredo dos códigos, `LEGACY_PEPPER`, ele cria sozinho.)
4. Quando ficar **Live**, abra `https://legacy-conta.onrender.com/saude` (o endereço aparece no topo): tem que
   mostrar `{"ok":true,"banco":"postgres"}`.
5. **Settings** → **Custom Domains** → **Add** → `account.legacyengine.com.br`. Ele mostra um destino tipo
   `legacy-conta.onrender.com` para um registro **CNAME**.

## 4. DNS do domínio
Como o site e o e-mail estão na Hostinger, o DNS deve estar lá: hPanel → **Domínios** → legacyengine.com.br →
**DNS / Nameservers** → **Gerenciar registros DNS**. (Se não estiver, fica no Registro.br → DNS → Editar zona.)
Não apague os registros que já existem (são os do site e da caixa de e-mail). Só adicione:
- os registros do **Resend** (passo 2.3), exatamente como ele mostra;
- **CNAME** `account` → o destino do Render (passo 3.5).

Pronto: `https://account.legacyengine.com.br/saude` responde, e o launcher funciona em qualquer PC.

## Opcional: servidor sempre acordado
No plano grátis o Render dorme sem uso, e o primeiro acesso depois disso leva ~1 min (o launcher mostra
"Acordando o servidor..."). Para evitar: cron-job.org (grátis) → chamar `https://account.legacyengine.com.br/saude`
a cada 10 minutos.
