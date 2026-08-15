const mongoose = require('mongoose');

// Connect to MongoDB
async function connectDB(uri) {
    try {
        await mongoose.connect(uri);
        console.log('Connected to MongoDB Atlas.');
        
        // Ensure balance document exists
        const count = await Balance.countDocuments();
        if (count === 0) {
            await Balance.create({ total_locked: 0 });
        }
    } catch (err) {
        console.error('MongoDB connection error:', err);
        process.exit(1);
    }
}

// Transaction Schema
const transactionSchema = new mongoose.Schema({
    discord_user_id: { type: String, required: true },
    amount: { type: Number, required: true },
    status: { type: String, default: 'pending' },
    payment_id: { type: String, unique: true },
    item_name: { type: String },
    thread_id: { type: String },
    created_at: { type: Date, default: Date.now }
});

const Transaction = mongoose.model('Transaction', transactionSchema);

// Balance Schema
const balanceSchema = new mongoose.Schema({
    total_locked: { type: Number, default: 0 }
});

const Balance = mongoose.model('Balance', balanceSchema);

module.exports = { connectDB, Transaction, Balance };
