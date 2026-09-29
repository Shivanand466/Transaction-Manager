# Transaction Manager

An interactive C program for managing energy transactions between buyers and sellers.

## Build

Requires GCC or another C11-compatible compiler.

```sh
gcc -std=c11 -O2 -Wall -Wextra -pedantic Transaction_System.c -o transaction_manager
```

## Run

Run the executable from the repository directory so it can find the data files:

```sh
./transaction_manager
```

On Windows, run `transaction_manager.exe` from PowerShell or Command Prompt.

## Data files

- `transactions.txt` stores transaction records as CSV with columns `transaction_id,buyer_id,seller_id,energy,price,timestamp`. Existing records load at startup and newly saved transactions are appended.
- `sellers.txt` stores seller rates as CSV with columns `seller_id,rate_upto_300,rate_above_300`.

Keep these files in the program's working directory. If `transactions.txt` is missing, the program starts with no saved transactions.

## Features

- Add and list transactions, and view records by seller or buyer.
- Filter transactions by date range or energy range.
- Calculate seller revenue and sort buyers or seller/buyer pairs by activity.
- Apply seller rates based on energy amount and flag regular buyer relationships.