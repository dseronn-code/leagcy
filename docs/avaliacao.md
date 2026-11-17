# Legacy: avaliação do projeto (US$)

Data-base: 16/11/2026

## Resultado

O custo de reposição do projeto Legacy é **US$ 4,27 milhões** (US$ 4.274.362): quanto custaria a um empregador nos EUA pagar uma equipe para reconstruir o mesmo software, com salários e encargos. O cálculo usa o modelo COCOMO II.2000 com seus fatores de ajuste, o tamanho medido do código e salários oficiais dos EUA.

| Dado | Valor | Fonte |
| --- | --- | --- |
| Tamanho do código | 37.911 linhas lógicas | Contagem no repositório, 16/11/2026 |
| Esforço | 225,1 meses de engenharia | COCOMO II.2000 com fatores de escala e multiplicadores de esforço (seção Premissas) |
| Prazo | 20,3 meses, equipe média de 11,1 pessoas | COCOMO II: 3,67 × 225,1^0,3162 |
| Salário, motor e jogo | US$ 171.980 por ano | Percentil 75 de desenvolvedores de software nos EUA, BLS |
| Salário, demais componentes | US$ 135.980 por ano | Mediana de desenvolvedores de software nos EUA, BLS |
| Encargos do empregador | × 1,4 sobre o salário | BLS ECEC: benefícios são 29,7% do custo total do empregador (março de 2025) |
| **Custo de reposição** | **US$ 4.274.362** | Soma por componente, abaixo |

## O que o número significa

Custo de reposição é quanto custaria uma equipe reconstruir o mesmo software. Não é preço de venda nem valor de mercado: esses dependem de comprador, negociação, usuários e receita. Só em salários, sem encargos, o valor é US$ 3.053.116.

## Tamanho medido

Linhas lógicas são as linhas de código sem comentários e sem linhas em branco, a medida que o COCOMO usa. O repositório tem 45.196 linhas físicas.

| Componente | Conteúdo | Linhas lógicas |
| --- | --- | --- |
| Motor e jogo | C++ do motor (19.345), shaders GLSL (5.759), scripts Lua (568) | 25.672 |
| Pipeline de conteúdo | Ferramentas e geradores em Python (9.579), agente de build (226), gerador da marca (116) | 9.921 |
| Plataforma | Launcher (1.222), servidor de contas (551), site (545) | 2.318 |
| **Total** | | **37.911** |

## Premissas do modelo

O COCOMO II ajusta o esforço de duas formas: fatores de escala, que valem para o projeto inteiro, e multiplicadores de esforço, que valem para cada componente. Os números vêm das tabelas oficiais do modelo; o nível escolhido para cada fator é decisão do avaliador e está descrito abaixo para conferência.

**Fatores de escala.** A soma é 18,10, o que dá o expoente 0,91 + 0,01 × 18,10 = 1,091.

| Fator | Nível | Valor | Motivo |
| --- | --- | --- | --- |
| PREC (precedentes) | Baixo | 4,96 | Motor próprio com ray tracing, sem base anterior |
| FLEX (flexibilidade) | Alto | 2,03 | Requisitos definidos pela própria equipe |
| RESL (arquitetura e riscos) | Nominal | 4,24 | Valor padrão |
| TEAM (coesão da equipe) | Alto | 2,19 | Equipe pequena e única |
| PMAT (maturidade do processo) | Nominal | 4,68 | Valor padrão |
| **Soma** | | **18,10** | |

Esforço sem multiplicadores: 2,94 × 37,911^1,091 = 155,2 meses, dividido entre os componentes pelo tamanho.

**Multiplicadores de esforço.**

| Componente | Multiplicadores | Produto | Esforço base | Esforço final |
| --- | --- | --- | --- | --- |
| Motor e jogo | CPLX muito alta 1,34 (renderização e física) × TIME alta 1,11 (tempo real) × RUSE alta 1,07 (motor reutilizável) | 1,592 | 105,1 | 167,2 |
| Pipeline de conteúdo | CPLX alta 1,17 (geometria procedural) | 1,170 | 40,6 | 47,5 |
| Plataforma | RELY alta 1,10 (contas e segurança) | 1,100 | 9,5 | 10,4 |
| **Total** | | | **155,2** | **225,1** |

**Salários e encargos.** O motor usa o percentil 75 porque renderização em tempo real e ray tracing exigem engenheiros mais experientes que a média. Os encargos seguem a pesquisa ECEC do BLS: em março de 2025, salários eram 70,3% do custo do empregador e benefícios 29,7%, ou seja, custo total = salário ÷ 0,703 = salário × 1,42. O cálculo usa 1,4.

## Custo por componente

| Componente | Esforço (meses) | Custo mensal por pessoa | Custo |
| --- | --- | --- | --- |
| Motor e jogo | 167,2 | US$ 20.064 (171.980 ÷ 12 × 1,4) | US$ 3.355.142 |
| Pipeline de conteúdo | 47,5 | US$ 15.864 (135.980 ÷ 12 × 1,4) | US$ 753.665 |
| Plataforma | 10,4 | US$ 15.864 | US$ 165.555 |
| **Total** | **225,1** | | **US$ 4.274.362** |

## O que não está incluído

- Bibliotecas de terceiros, baixadas na compilação
- Marca, domínio, empresa, infraestrutura em produção, downloads e usuários
- Desconto de risco ou de mercado: um comprador pode aplicar o seu, e o preço de venda pode ficar abaixo ou acima deste valor

## Fontes

- [Overview of COCOMO II, Universidade do Texas em Arlington](https://ranger.uta.edu/~khalili/Overview%20of%20COCOMO.htm): constantes 2,94 e 3,67, tabelas de fatores de escala e multiplicadores de esforço
- [Bureau of Labor Statistics, API pública, série OEUN000000000000015125213](https://api.bls.gov/publicAPI/v1/timeseries/data/OEUN000000000000015125213): mediana anual de desenvolvedores de software nos EUA, US$ 135.980
- [Bureau of Labor Statistics, API pública, série OEUN000000000000015125214](https://api.bls.gov/publicAPI/v1/timeseries/data/OEUN000000000000015125214): percentil 75 anual de desenvolvedores de software nos EUA, US$ 171.980
- [BLS, Employer Costs for Employee Compensation, março de 2025](https://www.bls.gov/opub/ted/2025/compensation-costs-for-private-industry-averaged-45-38-per-hour-worked-in-march-2025.htm): salários 70,3% e benefícios 29,7% do custo do empregador
- Contagem de linhas: repositório privado do projeto, 16/11/2026

Fontes consultadas em 17/11/2026.
