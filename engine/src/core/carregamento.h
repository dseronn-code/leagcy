#pragma once
// Progresso da carga da cena, para a tela de carregamento (main.cpp desenha; os laços longos avisam).
//
// Sem isto a janela ficava branca e o Windows marcava "Não está respondendo" enquanto a cena carregava (a Times
// Square leva minutos na primeira vez, montando o cache): a carga roda inteira na thread principal, sem atender a
// janela. Os laços longos da carga (modelos, malhas, BLAS, texturas) chamam estas funções; a tela é redesenhada (e os
// eventos da janela atendidos) no máximo ~30 vezes por segundo, só na thread que ligou a tela.
#include <functional>
#include <string>

namespace carregamento {

using Desenho = std::function<void(float fracao)>;  // fração 0..1 do progresso total

void ligar(Desenho desenho);  // na thread principal, antes de carregar
void desligar();
// Progresso absoluto (0..1). Só avança (nunca volta) e só redesenha a cada ~33 ms.
void progresso(float fracao);
// Um modelo glTF terminou de carregar (fase 1, sem total conhecido: avança devagar até 50%).
void modeloCarregado();
// Etapa i de n da fase 2 (preparar malhas e texturas na GPU): 50% → 95%.
void etapa(size_t i, size_t n);
// Só manter a janela viva (laços longos sem progresso medido).
void pulso();
// Progresso dentro do modelo que está carregando (0..1): avança um pouco até o próximo modeloCarregado(), para a
// barra não ficar parada no 1º modelo grande (a cidade).
void dentroDoModelo(float f);
// Roda `trabalho` em até 16 threads (a principal inclusive) e espera todas; enquanto espera, a thread principal
// continua redesenhando a tela (antes ela ficava parada no join() e o Windows dava "Não está respondendo").
void emParalelo(const std::function<void()>& trabalho, size_t itens = ~size_t(0));

// "Cozinhar" o conteúdo (como o cook da Unreal): com a variável de ambiente LEGACY_COZINHAR=<arquivo>, cada arquivo
// do cache (texcache/: BC7, tangentes, geometria virtualizada) que a carga usar é anotado nesse arquivo, um por linha.
// O gerador de instaladores roda cada cena uma vez assim e manda só esses arquivos no download: o jogador já recebe a
// carga pronta (sem isso, a 1ª abertura depois de instalar ou atualizar calculava tudo no PC dele).
void cacheUsado(const std::string& nome);
// Pasta do cache: "texcache" (ao lado de onde o jogo roda) ou a de LEGACY_CACHE (a cozinha usa o cache quente do
// motor no PC que gera os instaladores, sem copiar nada).
std::string pastaCache();

// Da cena .json: imagem da tela de carregamento e ícone da janela ("carregamento" e "icone", caminhos relativos à
// pasta do motor ou à pasta da cena). Vazio se a cena não define.
std::string imagemDaCena(const std::string& cena, const char* chave);
// Texto de uma chave da cena .json (ex.: "titulo", o nome da janela e da barra de tarefas). Vazio se não houver.
std::string textoDaCena(const std::string& cena, const char* chave);

}  // namespace carregamento
