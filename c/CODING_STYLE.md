# Estilo do código C

O objetivo é permitir que alguém leia o projeto sem precisar reconstruir mentalmente
ownership, estados e efeitos colaterais.

## Comentários

- Comentários explicam **por que** uma decisão existe, não traduzem sintaxe C.
- Ownership, lifetime, invariantes e cleanup devem ser documentados onde não forem óbvios.
- Fluxos de segurança (temporário -> validação -> publicação) devem permanecer explícitos.
- Processos externos e concorrência devem explicar a fronteira entre threads/processos.
- Evite comentários que só repetem o nome da função ou a linha seguinte.

## Formatação

- C17, quatro espaços de indentação e chaves visíveis em blocos não triviais.
- Uma operação de cleanup por linha quando várias regiões de memória são liberadas.
- Evite esconder controle de fluxo importante em uma linha.
- Nomes locais devem descrever o papel do valor, não apenas o tipo.
- Funções grandes devem ser divididas por etapas lógicas e acompanhadas de comentários de fluxo.

## Memória

- `*_init` cria estado vazio válido.
- `*_clear` libera ownership e devolve a struct a um estado vazio reutilizável.
- `*_copy` faz cópia profunda, salvo comentário explícito em contrário.
- `const char *` de eventos e getters normalmente é emprestado.
- APIs que transferem ownership precisam dizer quem libera o resultado.
