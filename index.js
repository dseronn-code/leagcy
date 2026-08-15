require('dotenv').config();
const { Client, GatewayIntentBits, REST, Routes, EmbedBuilder, AttachmentBuilder, ActionRowBuilder, ButtonBuilder, ButtonStyle, ChannelType, PermissionFlagsBits } = require('discord.js');
const express = require('express');
const { connectDB, Transaction, Balance } = require('./mongo');
const { initMP, createPixPayment, isPaymentApproved } = require('./payment');

const app = express();
app.use(express.json());

const DISCORD_TOKEN = process.env.DISCORD_TOKEN;
const MP_ACCESS_TOKEN = process.env.MERCADO_PAGO_ACCESS_TOKEN;
const WEBHOOK_URL = process.env.WEBHOOK_URL;
const MONGO_URI = process.env.MONGO_URI;
const PORT = process.env.PORT || 3000;

// O ID do admin que vai supervisionar as salas
const ADMIN_ID = '990984399145275412';

if (!DISCORD_TOKEN || !MP_ACCESS_TOKEN || !MONGO_URI) {
    console.error("Missing required environment variables (DISCORD_TOKEN, MERCADO_PAGO_ACCESS_TOKEN, or MONGO_URI).");
    process.exit(1);
}

// Connect to MongoDB
connectDB(MONGO_URI);
initMP(MP_ACCESS_TOKEN);

const client = new Client({
    intents: [GatewayIntentBits.Guilds, GatewayIntentBits.GuildMessages]
});

// Setup Express Webhook & Ping Endpoint
app.get('/ping', (req, res) => {
    res.send('pong'); // UptimeRobot will ping this
});

app.post('/webhook', async (req, res) => {
    const paymentId = req.query?.['data.id'] || req.body?.data?.id;
    
    if (paymentId) {
        try {
            const approved = await isPaymentApproved(paymentId);
            if (approved) {
                // Find transaction in Mongo
                const tx = await Transaction.findOne({ payment_id: paymentId, status: 'pending' });
                if (tx) {
                    // Update status
                    tx.status = 'paid';
                    await tx.save();

                    // Increment balance
                    await Balance.updateOne({}, { $inc: { total_locked: tx.amount } });

                    console.log(`Payment ${paymentId} confirmed for R$ ${tx.amount}`);

                    try {
                        // Find the checkout thread
                        const thread = await client.channels.fetch(tx.thread_id);
                        if (thread && thread.parent) {
                            // Notify in checkout thread
                            await thread.send('✅ **Pagamento confirmado!** Gerando a sua key em um novo tópico de entrega...');
                            await thread.setArchived(true);

                            // Create delivery thread
                            const deliveryThread = await thread.parent.threads.create({
                                name: `🔑-key-${tx.discord_user_id}`,
                                autoArchiveDuration: 60,
                                type: ChannelType.PrivateThread,
                                reason: 'Entrega de Key'
                            });

                            // Add user and admin
                            await deliveryThread.members.add(tx.discord_user_id);
                            await deliveryThread.members.add(ADMIN_ID);

                            // Send the key
                            const embed = new EmbedBuilder()
                                .setTitle(`🎉 Compra Concluída: ${tx.item_name}`)
                                .setDescription(`Muito obrigada pela compra! Aqui está a sua Key de ativação exclusiva:\n\n\`\`\`56345-36367-34265\`\`\``)
                                .setColor('#00ff00');
                            
                            await deliveryThread.send({ content: `<@${tx.discord_user_id}> <@${ADMIN_ID}>`, embeds: [embed] });
                        }
                    } catch (e) {
                        console.error("Error creating delivery thread:", e);
                    }
                }
            }
        } catch (error) {
            console.error('Webhook Error:', error);
        }
    }
    res.sendStatus(200);
});

// Slash Commands
const commands = [
    {
        name: 'setup_loja',
        description: 'Admin: Criar painel de vendas com botão no canal atual',
        options: [
            { name: 'title', type: 3, description: 'Título do Produto', required: true },
            { name: 'price', type: 10, description: 'Preço em BRL', required: true },
            { name: 'description', type: 3, description: 'Descrição do produto', required: false },
            { name: 'image', type: 3, description: 'URL da Imagem do banner', required: false },
            { name: 'color', type: 3, description: 'Cor do painel em HEX (ex: #ff0000)', required: false }
        ]
    },
    { name: 'balance', description: 'Admin: Check locked funds' },
    {
        name: 'withdraw',
        description: 'Admin: Clear funds after withdrawing from Mercado Pago',
        options: [{ name: 'amount', type: 10, description: 'Amount withdrawn', required: true }]
    }
];

client.once('ready', async () => {
    console.log(`Logged in as ${client.user.tag}!`);
    const rest = new REST({ version: '10' }).setToken(DISCORD_TOKEN);
    try {
        await rest.put(Routes.applicationCommands(client.user.id), { body: commands });
        console.log('Successfully reloaded application (/) commands.');
    } catch (error) {
        console.error(error);
    }
});

client.on('interactionCreate', async interaction => {
    if (interaction.isChatInputCommand()) {
        if (interaction.commandName === 'setup_loja') {
            if (!interaction.memberPermissions.has(PermissionFlagsBits.ManageGuild) && interaction.user.id !== ADMIN_ID) {
                return interaction.reply({ content: 'Você não tem permissão.', flags: 64 });
            }

            const title = interaction.options.getString('title');
            const price = interaction.options.getNumber('price');
            const desc = interaction.options.getString('description') || 'Leia sobre o produto aqui de forma super profissional.';
            const imageUrl = interaction.options.getString('image');
            const color = interaction.options.getString('color') || '#5865F2';

            if (price < 0.01) {
                return interaction.reply({ content: 'O valor mínimo é R$ 0.01', flags: 64 });
            }

            const embed = new EmbedBuilder()
                .setTitle(`🛒 ${title}`)
                .setDescription(`${desc}\n\n**Valor:** R$ ${price.toFixed(2)}\n\nClique no botão abaixo para comprar e gerar seu PIX!`)
                .setColor(color);
                
            if (imageUrl) {
                try {
                    embed.setImage(imageUrl);
                } catch (e) {
                    console.log("Invalid image URL provided.");
                }
            }

            const customId = `buy_${Buffer.from(title).toString('base64')}_${price}`;

            const button = new ButtonBuilder()
                .setCustomId(customId)
                .setLabel('Comprar Produto')
                .setStyle(ButtonStyle.Success)
                .setEmoji('🛒');

            const row = new ActionRowBuilder().addComponents(button);

            await interaction.channel.send({ embeds: [embed], components: [row] });
            await interaction.reply({ content: 'Loja configurada com sucesso!', flags: 64 });
        }

        if (interaction.commandName === 'balance') {
            if (!interaction.memberPermissions.has(PermissionFlagsBits.ManageGuild) && interaction.user.id !== ADMIN_ID) return;
            const bal = await Balance.findOne();
            interaction.reply({ content: `💰 Total Locked Funds: **R$ ${(bal ? bal.total_locked : 0).toFixed(2)}**`, flags: 64 });
        }

        if (interaction.commandName === 'withdraw') {
            if (!interaction.memberPermissions.has(PermissionFlagsBits.ManageGuild) && interaction.user.id !== ADMIN_ID) return;
            const withdrawAmount = interaction.options.getNumber('amount');
            const bal = await Balance.findOne();
            const currentBalance = bal ? bal.total_locked : 0;
            
            if (withdrawAmount > currentBalance) {
                return interaction.reply({ content: `Cannot withdraw R$ ${withdrawAmount}. Current balance is R$ ${currentBalance}.`, flags: 64 });
            }
            
            await Balance.updateOne({}, { $inc: { total_locked: -withdrawAmount } });
            interaction.reply({ content: `✅ Successfully withdrew **R$ ${withdrawAmount.toFixed(2)}**`, flags: 64 });
        }
    }

    if (interaction.isButton()) {
        if (interaction.customId.startsWith('buy_')) {
            await interaction.deferReply({ flags: 64 });

            const parts = interaction.customId.split('_');
            const item = Buffer.from(parts[1], 'base64').toString('utf8');
            const amount = parseFloat(parts[2]);

            try {
                const thread = await interaction.channel.threads.create({
                    name: `🛒-checkout-${interaction.user.username}`,
                    autoArchiveDuration: 60,
                    type: ChannelType.PrivateThread,
                    reason: 'Processo de Compra'
                });

                await thread.members.add(interaction.user.id);
                await thread.members.add(ADMIN_ID);

                await interaction.editReply(`✅ Seu ticket de compra privado foi criado: <#${thread.id}>`);

                const dummyEmail = `user${interaction.user.id}@discordapp.com`;
                const pixData = await createPixPayment(amount, dummyEmail, `Compra: ${item}`, WEBHOOK_URL);

                // Save to MongoDB
                await Transaction.create({
                    discord_user_id: interaction.user.id,
                    amount: amount,
                    payment_id: pixData.id,
                    item_name: item,
                    thread_id: thread.id
                });

                const qrBuffer = Buffer.from(pixData.qr_code_base64, 'base64');
                const qrAttachment = new AttachmentBuilder(qrBuffer, { name: 'qrcode.png' });

                const embed = new EmbedBuilder()
                    .setTitle(`Pagamento PIX: ${item}`)
                    .setDescription(`Valor: **R$ ${amount.toFixed(2)}**\n\nCopie o código abaixo ou escaneie o QR Code. Assim que o pagamento for feito, eu vou confirmar automaticamente!`)
                    .addFields({ name: 'PIX Copia e Cola', value: `\`\`\`\n${pixData.qr_code}\n\`\`\`` })
                    .setImage('attachment://qrcode.png')
                    .setColor('#00ff00');

                await thread.send({ content: `<@${interaction.user.id}> <@${ADMIN_ID}>`, embeds: [embed], files: [qrAttachment] });

            } catch (error) {
                console.error('Button error:', error);
                await interaction.editReply('Erro ao criar o ticket de compra. Verifique se o servidor permite Tópicos Privados e se o bot tem permissão.');
            }
        }
    }
});

app.listen(PORT, () => {
    console.log(`Webhook & Ping server is running on port ${PORT}`);
});

client.login(DISCORD_TOKEN);
