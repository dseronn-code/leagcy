// Servidor do jogo "Aventura dos Bichos": serve a página e vende estrelas por PIX.
// Uso: npm run jogo  (precisa de MERCADO_PAGO_ACCESS_TOKEN no .env)
require('dotenv').config();
const path = require('path');
const express = require('express');
const { initMP, createPixPayment, isPaymentApproved } = require('../payment');

const MP_ACCESS_TOKEN = process.env.MERCADO_PAGO_ACCESS_TOKEN;
const PORT = process.env.JOGO_PORT || 3001;

if (!MP_ACCESS_TOKEN) {
    console.error('Missing MERCADO_PAGO_ACCESS_TOKEN.');
    process.exit(1);
}
initMP(MP_ACCESS_TOKEN);

// O preço é definido aqui no servidor, nunca pelo navegador.
const PACOTES = {
    p10: { estrelas: 10, valor: 1.99 },
    p30: { estrelas: 30, valor: 4.99 },
    p80: { estrelas: 80, valor: 9.99 },
};

// Pedidos em memória: payment_id -> { estrelas, entregue }
const pedidos = new Map();

const app = express();
app.use(express.json());
app.use(express.static(__dirname, { index: 'index.html' }));

app.post('/api/estrelas/pix', async (req, res) => {
    const pacote = PACOTES[req.body?.pacote];
    if (!pacote) return res.status(400).json({ error: 'Pacote inválido' });

    try {
        const pix = await createPixPayment(
            pacote.valor,
            'jogador@aventuradosbichos.app',
            `Aventura dos Bichos: ${pacote.estrelas} estrelas`,
            process.env.JOGO_WEBHOOK_URL
        );
        pedidos.set(String(pix.id), { estrelas: pacote.estrelas, entregue: false });
        res.json({ id: pix.id, estrelas: pacote.estrelas, qr_code: pix.qr_code, qr_code_base64: pix.qr_code_base64 });
    } catch (error) {
        res.status(502).json({ error: 'Erro ao gerar PIX' });
    }
});

app.get('/api/estrelas/pix/:id', async (req, res) => {
    const pedido = pedidos.get(req.params.id);
    if (!pedido) return res.status(404).json({ error: 'Pedido não encontrado' });
    if (pedido.entregue) return res.json({ status: 'approved', estrelas: 0 });

    const approved = await isPaymentApproved(req.params.id);
    if (!approved) return res.json({ status: 'pending' });

    // Entrega as estrelas uma única vez por pagamento.
    pedido.entregue = true;
    res.json({ status: 'approved', estrelas: pedido.estrelas });
});

app.listen(PORT, () => console.log(`Aventura dos Bichos em http://localhost:${PORT}`));
