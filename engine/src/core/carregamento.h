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

// Da cena .json: imagem da tela de carregamento e ícone da janela ("carregamento" e "icone", caminhos relativos à
// pasta do motor ou à pasta da cena). Vazio se a cena não define.
std::string imagemDaCena(const std::string& cena, const char* chave);

}  // namespace carregamento
