-- ETAPA 1 - Testes de segurança
-- Rodar UM bloco por vez no SQL Editor (selecione as linhas e clique em Run).
-- Troque '1234' pelo seu PIN, se mudou.

-- Teste 1: ler partidas como anon -> deve funcionar (lista vazia no início)
set role anon;
select * from public.partidas;
reset role;

-- Teste 2: ler o PIN como anon -> deve dar "permission denied"
set role anon;
select * from public.segredos;
reset role;

-- Teste 3: inserir direto como anon -> deve dar "permission denied"
set role anon;
insert into public.partidas (modo, time_a, time_b) values ('futebol', 'A', 'B');
reset role;

-- Teste 4: PIN errado -> deve dar "PIN incorreto"
set role anon;
select public.enviar_partida('0000', 'futebol', 'GREMIO', 'INTER');
reset role;

-- Teste 5: PIN certo -> deve retornar o id da partida (ex.: 1)
set role anon;
select public.enviar_partida('1234', 'futebol', 'GREMIO', 'INTER');
reset role;

-- Teste 6: modo inválido -> deve dar erro de "check constraint"
set role anon;
select public.enviar_partida('1234', 'xadrez', 'A', 'B');
reset role;

-- Teste 7: nome com mais de 12 letras -> deve dar erro de "check constraint"
set role anon;
select public.enviar_partida('1234', 'futebol', 'NOME MUITO GRANDE', 'B');
reset role;

-- Conferir o que ficou gravado (só deve ter a partida do teste 5)
select * from public.partidas;

-- Para trocar o PIN depois:
-- update public.segredos set pin = 'novo' where placar_id = 'placar01';
