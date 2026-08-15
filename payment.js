const { MercadoPagoConfig, Payment } = require('mercadopago');

let client;
let paymentAPI;

// Initialize the Mercado Pago SDK
function initMP(accessToken) {
    client = new MercadoPagoConfig({ accessToken: accessToken, options: { timeout: 5000 } });
    paymentAPI = new Payment(client);
    console.log('Mercado Pago SDK initialized.');
}

/**
 * Creates a PIX payment in Mercado Pago
 * @param {number} amount 
 * @param {string} email - Optional dummy email, MP requires it
 * @param {string} description 
 * @param {string} webhookUrl 
 * @returns {Promise<Object>} The PIX data
 */
async function createPixPayment(amount, email, description, webhookUrl) {
    if (!paymentAPI) throw new Error('Mercado Pago SDK not initialized.');

    const paymentData = {
        body: {
            transaction_amount: amount,
            description: description,
            payment_method_id: 'pix',
            payer: {
                email: email
            },
            notification_url: webhookUrl
        }
    };

    try {
        const response = await paymentAPI.create(paymentData);
        return {
            id: response.id,
            qr_code: response.point_of_interaction.transaction_data.qr_code,
            qr_code_base64: response.point_of_interaction.transaction_data.qr_code_base64
        };
    } catch (error) {
        console.error('Error creating PIX:', error);
        throw error;
    }
}

/**
 * Verifies if a payment by ID is approved
 * @param {number|string} paymentId 
 * @returns {Promise<boolean>}
 */
async function isPaymentApproved(paymentId) {
    if (!paymentAPI) throw new Error('Mercado Pago SDK not initialized.');

    try {
        const response = await paymentAPI.get({ id: paymentId });
        return response.status === 'approved';
    } catch (error) {
        console.error('Error checking payment:', error);
        return false;
    }
}

module.exports = {
    initMP,
    createPixPayment,
    isPaymentApproved
};
