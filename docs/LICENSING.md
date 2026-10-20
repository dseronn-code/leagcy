# Sonora — Plano de licenciamento

> Este documento organiza o caminho e o que já está pronto no código. **Não é aconselhamento jurídico.** Contratos de música envolvem direitos autorais, tributação e regras que mudam por país; tudo aqui deve ser validado com um advogado especializado em direito autoral/entretenimento antes de assinar qualquer coisa.

## 1. O que precisa ser licenciado

Cada música tem **dois conjuntos de direitos**, e um app de streaming precisa dos dois:

| Direito | De quem | Quem licencia |
|---|---|---|
| **Fonograma** (a gravação) | Gravadora / artista independente | Majors (Universal, Sony, Warner), agregadores de independentes (ex.: Merlin) e distribuidoras digitais (ex.: The Orchard, Believe, ONErpm, CD Baby, DistroKid) |
| **Obra musical** (letra e melodia) | Compositores e editoras | Editoras diretamente e/ou entidades de gestão coletiva |

No Brasil, a cobrança de **execução pública** é feita pelo **ECAD**, e as **editoras** (muitas reunidas na **UBEM**) negociam os direitos de reprodução das obras. O enquadramento exato do streaming interativo nessas duas frentes já foi e continua sendo discutido nos tribunais e no Congresso (há projeto de lei específico para streaming), então **confirme com o advogado qual é a prática vigente** antes de fechar valores.

Fora do Brasil cada território tem suas entidades (nos EUA, por exemplo, licença mecânica coletiva e sociedades de execução pública; na Europa, sociedades nacionais e hubs pan-europeus). Por isso o lançamento costuma começar **por um país**.

## 2. Três caminhos

### A. Parceiro B2B com catálogo já licenciado (mais rápido)
Empresas de "streaming como serviço" já têm contratos com gravadoras e entregam o catálogo por API. Um exemplo concreto: a **Tuned Global** anunciou um acordo global com a Universal Music para o catálogo gravado da UMG, disponível aos clientes da plataforma ([Music Business Worldwide](https://www.musicbusinessworldwide.com/universal-strikes-global-licensing-deal-with-b2b-music-streaming-tech-provider-tuned-global/)). A documentação deles descreve exatamente o que o Sonora implementou: **URL de stream assinada** validando assinatura e direitos por dispositivo, e **registro de reprodução para royalties** ([guia rápido da API](https://docs.tunedglobal.com/api-quick-start-guide), [visão geral](https://docs.tunedglobal.com/)).

- **Prós:** meses em vez de anos; contratos, ingestão de metadados, CDN e relatórios já resolvidos.
- **Contras:** margem do parceiro; dependência; o catálogo e os formatos disponíveis (hi-res, Atmos) dependem do que o parceiro tem; regras de uso restritivas (a Tuned Global, por exemplo, proíbe usar o catálogo para treinar IA salvo autorização).
- **Perguntas para fazer:** quais majors e independentes estão cobertos **no Brasil**; se os direitos de obra (editoras/ECAD) estão inclusos ou ficam com você; quais formatos (FLAC 24-bit, Atmos); DRM exigido; preço (fixo + por usuário); mínimos; prazo para lançar.

### B. Contratos diretos (mais controle, muito mais lento)
Negociar com cada major, com o Merlin (independentes), com distribuidoras e com editoras/entidades.
- Licenciantes costumam exigir **adiantamentos**, **garantia mínima por assinante**, participação na receita, auditoria e relatórios mensais.
- Na prática exige empresa constituída, capital para os adiantamentos, plano de negócio com projeção de assinantes e equipe jurídica. Spotify, Deezer e Tidal levaram anos nesse processo.

### C. Começar por um nicho (recomendado em paralelo)
Fechar com **distribuidoras de independentes** e selos que já entregam masters em hi-res (jazz, clássico, MPB, eletrônica), lançar só no Brasil e usar isso como tração para negociar com parceiro B2B ou majors. Combina bem com o diferencial de qualidade do Sonora.

**Recomendação:** A + C. Conversar com 2–3 parceiros B2B e, ao mesmo tempo, com 2–3 distribuidoras de independentes com catálogo hi-res. Um modelo de e-mail está no fim deste documento.

## 3. Pré-requisitos do negócio

- [ ] Empresa constituída (CNPJ) e conta bancária PJ.
- [ ] Advogado de direito autoral/entretenimento.
- [ ] **Modelo de receita.** Sem anúncios, a receita vem só de **assinatura** (com teste grátis opcional). É isso que os licenciantes vão querer ver: preço, planos (individual, família, estudante) e projeção de assinantes.
- [ ] Capital para adiantamentos/mínimos (rota B) ou para a mensalidade do parceiro (rota A).
- [ ] Política de privacidade e termos de uso (LGPD), incluindo o uso do histórico para relatórios de royalties.
- [ ] Contas de desenvolvedor na App Store e no Google Play com cobrança de assinaturas.

## 4. O que o código já cumpre

Implementado e testado (`packages/core/src/licensing.ts`, `apps/server/src/app.ts`, testes em `packages/core/test/licensing.test.ts` e `apps/server/test/licensing.test.ts`):

| Exigência típica do licenciante | Como está no Sonora |
|---|---|
| Só assinantes ouvem o catálogo licenciado | `authorize()` checa plano e validade; conteúdo CC/acervo próprio continua livre. |
| Respeitar território | Faixas trazem `territories` (ISO); a conta tem país; fora do território → `403 territory`. |
| Não expor URLs permanentes | Metadados de faixas licenciadas não trazem URL (`requiresAuthorization`); o app pede `GET /tracks/:id/stream` na hora de tocar e recebe URL assinada do provider (`streamSources`), com cache de no máximo 4 min no aparelho. |
| Offline com prazo | Download licenciado só com autorização (`purpose=download`); licença de **30 dias**; o app revalida ao sincronizar (`POST /downloads/renew`) e **apaga** faixas revogadas ou sem direito. Licença vencida não toca offline. |
| Reprodução conta a partir de 30 s | `isQualifiedPlay` (≥ 30 s ou 50% da faixa) é o critério de "stream" no relatório. |
| País e plano confiáveis no relatório | O **servidor** carimba país e plano em cada reprodução na ingestão; o cliente não consegue forjar. |
| Relatório mensal por gravação | `GET /admin/reports/usage?period=AAAA-MM&pool=BR:<centavos>` gera CSV por **ISRC × UPC × território × plano**, com streams, tempo ouvido e **rateio pró-rata** do valor de royalties (a soma bate exatamente com o valor informado). |
| Rotas administrativas protegidas | `/admin/*` só existe com `ADMIN_TOKEN` configurado; sem token responde 404. |
| UX honesta | Faixa indisponível é pulada e o mini-player avisa o motivo (sem assinatura, fora da região, retirada do catálogo). |

## 5. O que ainda falta (depende do contrato)

| Item | Por quê | Esforço |
|---|---|---|
| **DRM** (Widevine no Android, FairPlay no iOS) para streams e downloads | A maioria dos contratos com majors exige; o formato depende do parceiro (ex.: FLAC criptografado em CMAF/CENC). | Alto, nativo. Definir após saber o que o parceiro entrega. |
| **Cobrança integrada** | Hoje o plano é ajustado por `POST /admin/users/:id/entitlement`. Falta receber as notificações de servidor da App Store e do Google Play e chamar `setEntitlement`. | Médio. |
| **Formato DDEX DSR** | O CSV atual tem as colunas usuais; grandes licenciantes pedem o formato DDEX, que exige licença de implementação da DDEX ([DSR](https://ddex.net/?p=3667), [guia de perfis](https://kb.ddex.net/implementing-each-standard/best-practices-for-all-ddex-standards/deals-and-commercial-aspects/reporting-sales-or-usages-for-individual-tracks)). | Médio. |
| **Ingestão de metadados DDEX ERN** | Rota B: gravadoras entregam lançamentos, territórios e datas por feed ERN. Na rota A o parceiro já resolve. | Médio/alto. |
| **Antifraude de streams** | Detectar padrões robóticos (loops, contas fazenda) antes de reportar. | Médio. |
| **País pela rede** | Hoje o país vem do cadastro ou do cabeçalho da CDN; alguns contratos pedem checagem por IP a cada sessão. | Baixo. |
| **Limite de dispositivos offline** | Alguns contratos limitam aparelhos com downloads por conta. | Baixo. |
| Adapter do provider escolhido | Implementar `CatalogProvider` + `streamSources` com a API real do parceiro. | Baixo/médio, depois do contrato. |

## 6. Roteiro sugerido

1. **Semana 1–2:** constituir/organizar a empresa, contratar advogado, definir preço e planos, montar apresentação (diferencial: hi-res sem anúncios, estatísticas, tecnologia pronta).
2. **Semana 2–6:** conversas com parceiros B2B e distribuidoras de independentes; pedir propostas e minutas.
3. **Após a escolha:** implementar o adapter do provider, DRM no formato que ele exigir e a cobrança; validar relatórios com o parceiro.
4. **Lançamento piloto no Brasil**, com o catálogo que já estiver licenciado e o acervo CC/Subsonic convivendo no app.

## 7. Modelo de e-mail para parceiros

> **Assunto:** Licenciamento de catálogo para app de streaming hi-res sem anúncios (Brasil)
>
> Olá, [nome],
>
> Sou [seu nome], da [empresa]. Estamos lançando o **Sonora**, um app de streaming por assinatura, **sem anúncios**, focado em **áudio lossless e hi-res** (FLAC até 24-bit/192 kHz), para Android e iOS, começando pelo **Brasil**.
>
> O app e o backend já estão desenvolvidos e cumprem os requisitos usuais de licenciamento: autorização por assinatura e território, URLs de stream assinadas, licença offline de 30 dias com revalidação, e relatórios mensais por ISRC/UPC/território/plano com rateio de royalties.
>
> Gostaríamos de entender:
> 1. Quais catálogos (majors e independentes) vocês podem disponibilizar no Brasil, e em quais formatos (FLAC, hi-res, Dolby Atmos);
> 2. Se os direitos de obra/editoras estão inclusos ou ficam a nosso cargo;
> 3. Requisitos de DRM e de relatório;
> 4. Modelo comercial (valor fixo, por assinante, mínimos) e prazo típico de integração.
>
> Podemos marcar uma conversa de 30 minutos nas próximas semanas?
>
> Obrigado,
> [nome] — [cargo], [empresa] · [telefone] · [site]
