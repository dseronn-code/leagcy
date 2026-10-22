# Cena: IA de vídeo realista, 100% gratuita

Você escreve o que quer ver e a Cena gera um vídeo realista com pessoas, lugares, luz e movimento de câmera.

- **Custo zero.** O modelo é o **Wan 2.2**, de código aberto, com licença Apache 2.0, que permite uso comercial. A GPU vem do plano gratuito do Kaggle ou do Colab, ou do seu próprio PC. Nada de assinatura nem de cartão de crédito.
- **O "diretor".** Ele transforma o seu pedido, em português, num roteiro de câmera de cinema (lente, luz, movimento) e aplica um filtro contra o "visual de IA": pele de plástico, mãos deformadas, cara de 3D.
- **Interface própria.** Uma página web onde você digita, escolhe o estilo e a câmera e assiste o resultado. No Kaggle/Colab ela ganha um link público que abre até no celular.

## Jeito mais fácil: Kaggle (grátis, ~30 h de GPU por semana)

1. Crie uma conta em [kaggle.com](https://www.kaggle.com) e **verifique o telefone** (Settings → Phone verification). A verificação é grátis e libera GPU e internet.
2. **Create → New Notebook → File → Import Notebook** e envie o arquivo [`notebooks/cena.ipynb`](notebooks/cena.ipynb).
3. No painel direito: **Accelerator → GPU T4 x2** e **Internet → On**.
4. **Run → Run all**. Na célula "Opção 1" aparece um link `https://….gradio.live`: abra e use.

No **Google Colab** é igual: *Arquivo → Fazer upload de notebook*, *Ambiente de execução → GPU T4*, *Executar tudo*.

## No seu PC (com placa NVIDIA)

```bash
cd video-ai
pip install -r requirements.txt   # instale antes o PyTorch com CUDA: https://pytorch.org
python -m cena.app                # abre em http://127.0.0.1:7860
# ou direto pelo terminal:
python -m cena.cli "senhora rindo enquanto prepara café numa cozinha antiga" --estilo documentario --segundos 4
```

## Qualidade e tempo (expectativa honesta)

| Modo | Modelo | Saída | VRAM | Tempo por clipe de ~4 s |
|---|---|---|---|---|
| **Máxima** | Wan 2.2 TI2V-5B | até 1280×704, 24 fps | ~15 GB, com descarregamento para a RAM | T4 grátis: ~15–40 min · RTX 4090: ~5–10 min |
| **Rápida** | Wan 2.1 T2V-1.3B | 832×480, 16 fps | ~8 GB | T4 grátis: ~5–15 min · RTX 3060: ~5 min |

Os tempos são estimativas: ainda não foram medidos com esta configuração, porque o ambiente onde a Cena foi escrita não tem GPU.

- **GPU gratuita é lenta, mas o resultado é o mesmo modelo.** Comece em 480p e com 2–3 s para testar ideias. Quando gostar de um resultado, guarde a **semente** e gere em 720p.
- Clipes duram até ~5 s, o limite desses modelos. Para um filme, gere várias tomadas e monte numa edição. Os prompts do iPhone 30 em `marketing/iphone30/PROMPTS_IA.md` servem de exemplo.
- Mãos e textos ainda são o ponto fraco de qualquer IA de vídeo: gere 2–3 versões e escolha a melhor.

## Como escrever pedidos que parecem filmagem real

Seja concreto: **quem** (idade, aparência, roupa), **onde**, **fazendo o quê**, **luz** (sol da manhã, neon, vela) e **o que a câmera faz**.

> ❌ "Um homem feliz"
> ✅ "Homem de 30 anos, barba curta, camiseta cinza, rindo ao telefone numa varanda de São Paulo no fim de tarde, luz dourada lateral"

## Configurações extras (variáveis de ambiente)

| Variável | Para quê |
|---|---|
| `CENA_LORA` | Carrega um LoRA (personagem, estilo ou aceleração) compatível com o modelo, do Hugging Face ou de um arquivo local. |
| `CENA_MODELO_MAXIMA` / `CENA_MODELO_RAPIDA` | Troca o modelo de cada modo (ex.: uma versão mais nova do Wan, quando sair). |
| `CENA_DTYPE` | `bf16` ou `fp16`. O padrão escolhe sozinho: fp16 na T4, que não tem bf16 nativo; bf16 em RTX 30xx e mais novas. |

## Uso responsável

Vídeo realista gerado por IA deve ser **identificado como IA** ao publicar; YouTube, TikTok e Instagram exigem essa marcação. Não gere pessoas reais sem consentimento, nem conteúdo que se passe por notícia ou por anúncio oficial de uma marca.

## Desenvolvimento

```bash
pip install gradio numpy imageio imageio-ffmpeg pytest
python -m pytest -q                 # 12 testes, rodam sem GPU (modelo falso)
python tools/build_notebook.py      # regenera notebooks/cena.ipynb depois de mudar cena/
```
