#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <time.h>

#define KGRN "\033[0;32;32m"
#define KCYN "\033[0;36m"
#define KRED "\033[0;32;31m"
#define KYEL "\033[1;33m"
#define KBLU "\033[0;32;34m"
#define KCYN_L "\033[1;36m"
#define KBRN "\033[0;33m"
#define RESET "\033[0m"

#define QUEUESIZE 10

#define BUFFER_SIZE 10000

typedef struct workFunction
{
  void *(*work)(void *);
  void *arg;
} workFunc;

typedef struct
{
  workFunc buf[QUEUESIZE];

  long head, tail;
  int full, empty;
  pthread_mutex_t *mut;
  pthread_cond_t *notFull, *notEmpty;
} queue;

queue *fifo;
queue *queueInit(void);
void queueDelete(queue *q);
void queueAdd(queue *q, workFunc in);
void queueDel(queue *q, workFunc *out);
void *consumer();
void printInfoToFile(int index);
pthread_t con[10];

static const char *const reason_names[] = {
    "LEJPCB_CONSTRUCTED",
    "LEJPCB_DESTRUCTED",
    "LEJPCB_START",
    "LEJPCB_COMPLETE",
    "LEJPCB_FAILED",
    "LEJPCB_PAIR_NAME",
    "LEJPCB_VAL_TRUE",
    "LEJPCB_VAL_FALSE",
    "LEJPCB_VAL_NULL",
    "LEJPCB_VAL_NUM_INT",
    "LEJPCB_VAL_NUM_FLOAT",
    "LEJPCB_VAL_STR_START",
    "LEJPCB_VAL_STR_CHUNK",
    "LEJPCB_VAL_STR_END",
    "LEJPCB_ARRAY_START",
    "LEJPCB_ARRAY_END",
    "LEJPCB_OBJECT_START",
    "LEJPCB_OBJECT_END",
    "LEJPCB_OBJECT_END_PRE",
};

// The JSON paths/labels that we are interested in
static const char *const tok[] = {

    "data[].s", // symbol
    "data[].p", // price
    "data[].t", // timestrap
    "data[].v", // volume

};

static int destroy_flag = 0;


void *saveCandleSticksAndAvarage();

static void INT_HANDLER(int signo)
{
  destroy_flag = 1;
}

static int websocket_write_back(struct lws *wsi_in, char *str, int str_size_in)
{
  if (str == NULL || wsi_in == NULL)
    return -1;

  int n;
  int len;
  char *out = NULL;

  if (str_size_in < 1)
    len = strlen(str);
  else
    len = str_size_in;

  out = (char *)malloc(sizeof(char) * (LWS_SEND_BUFFER_PRE_PADDING + len + LWS_SEND_BUFFER_POST_PADDING));
  //* setup the buffer*/
  memcpy(out + LWS_SEND_BUFFER_PRE_PADDING, str, len);
  //* write out*/
  n = lws_write(wsi_in, out + LWS_SEND_BUFFER_PRE_PADDING, len, LWS_WRITE_TEXT);

  printf(KBLU "[websocket_write_back] %s\n" RESET, str);
  //* free the buffer*/
  free(out);

  return n;
}

struct tradingInfo
{
  float price;
  char symbol[40];
  unsigned long int timestrap;
  float volume;
};

struct tradingInfo tradingInfos[BUFFER_SIZE]; // buffer
unsigned tradingInfoIndex = 0;

static signed char
cb(struct lejp_ctx *ctx, char reason)
{

  static int counter = 0;

  if (reason & LEJP_FLAG_CB_IS_VALUE && (ctx->path_match > 0))
  {
    switch (counter)
    {
    case 0:
      tradingInfos[tradingInfoIndex].price = atof(ctx->buf);
      counter++;
      break;
    case 1:
      strcpy(tradingInfos[tradingInfoIndex].symbol, ctx->buf);
      counter++;
      break;
    case 2:
      tradingInfos[tradingInfoIndex].timestrap = strtoul(ctx->buf, NULL,10);
      counter++;
      break;
    case 3:
      tradingInfos[tradingInfoIndex].volume = atof(ctx->buf);

      pthread_mutex_lock(fifo->mut);

      while (fifo->full)
      {

        pthread_cond_wait(fifo->notFull, fifo->mut);
      }

      workFunc workpoint;
      workpoint.work = &(printInfoToFile);
      workpoint.arg = tradingInfoIndex;

      queueAdd(fifo, workpoint);

      pthread_mutex_unlock(fifo->mut);
      pthread_cond_signal(fifo->notEmpty);

      tradingInfoIndex = (tradingInfoIndex + 1) % BUFFER_SIZE;
      counter = 0;
      break;

    default:

      break;
    }
    
  }

  if (reason == LEJPCB_COMPLETE)
  {
    struct timeval tv;

    gettimeofday(&tv, NULL);
  }
  return NULL;
}

struct lejp_ctx ctx;

pthread_t TradingThread;

static int ws_service_callback(
    struct lws *wsi,
    enum lws_callback_reasons reason, void *user,
    void *in, size_t len)
{

  char *msg;

  switch (reason)
  {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    printf(KYEL "[Main Service] Connect with server success.\n" RESET);

    pthread_create(&TradingThread, NULL, saveCandleSticksAndAvarage, NULL);
    for (int i = 0; i < 10; i++)
    {
      pthread_create(&con[i], NULL, consumer, NULL);
    }
    lws_callback_on_writable(wsi);

    break;

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    printf(KRED "[Main Service] Connect with server error: %s.\n" RESET, in);
    destroy_flag = 1;
    break;

  case LWS_CALLBACK_CLOSED:
    printf(KYEL "[Main Service] LWS_CALLBACK_CLOSED\n" RESET);
    destroy_flag = 1;
    lejp_destruct(&ctx); // when the callback ends destroy destructor
    break;

  case LWS_CALLBACK_CLIENT_RECEIVE:
    printf(KCYN_L "[Main Service] Client recvived:%s\n" RESET, (char *)in);
    msg = (char *)in;

    lejp_construct(&ctx, cb, NULL, tok, LWS_ARRAY_SIZE(tok)); // of parser
    int m = lejp_parse(&ctx, (uint8_t *)msg, strlen(msg));
    if (m < 0 && m != LEJP_CONTINUE)
    {
        lwsl_err("parse failed %d\n", m);
    }


    break;
  case LWS_CALLBACK_CLIENT_WRITEABLE:
    printf(KYEL "[Main Service] On writeable is called.\n" RESET);
    char *out = NULL;

    int sym_num = 2;
    char symb_arr[4][100] = {"APPL\0", "AMZN\0", "BINANCE:BTCUSDT\0", "IC MARKETS:1\0"};
    char str[50];
    for (int i = 0; i < 4; i++)
    {
      sprintf(str, "{\"type\":\"subscribe\",\"symbol\":\"%s\"}", symb_arr[i]);

      // printf(str);
      int len = strlen(str);

      out = (char *)malloc(sizeof(char) * (LWS_SEND_BUFFER_PRE_PADDING + len + LWS_SEND_BUFFER_POST_PADDING));
      memcpy(out + LWS_SEND_BUFFER_PRE_PADDING, str, len);

      lws_write(wsi, out + LWS_SEND_BUFFER_PRE_PADDING, len, LWS_WRITE_TEXT);

      // printf("\\n");
    }
    free(out);
    break;

  default:
    break;
  }

  return 0;
}

static struct lws_protocols protocols[] =
    {
        {
            "example-protocol",
            ws_service_callback,
        },
        {NULL, NULL, 0, 0} /* terminator */
};

struct candleStick
{
  float initialPrice;
  float finalPrice;
  float maxPrice;
  float minPrice;
  float totalVolume;
  unsigned int transactions;
  float volumePrice;
};

void initialCandleStick(struct candleStick *candleStick, int size, int candleSticksIndex)
{
  int i = 0;
  for (i; i < size; i++)
  {
    candleStick[candleSticksIndex + i * 15].initialPrice = 0;
    candleStick[candleSticksIndex + i * 15].finalPrice = 0;
    candleStick[candleSticksIndex + i * 15].maxPrice = __FLT_MIN__;
    candleStick[candleSticksIndex + i * 15].minPrice = __FLT_MAX__;
    candleStick[candleSticksIndex + i * 15].totalVolume = 0;
    candleStick[candleSticksIndex + i * 15].transactions = 0;
    candleStick[candleSticksIndex + i * 15].volumePrice = 0;
    ;
  }
}

const char *symbols[] = {"APPL\0", "AMZN\0", "BINANCE:BTCUSDT\0", "IC MARKETS:1\0"};

void *saveCandleSticksAndAvarage()
{

  static int candleStickArrayFull = 0;

  static unsigned int tail = 0;

  static unsigned int candleSticksIndex = 0;

  struct tradingInfo data;

  struct candleStick candleSticks[4 * 15];

  struct tradingInfo finalTradingInfos[4];

  while (1)
  {
    initialCandleStick(candleSticks, 4, candleSticksIndex);
    float mean[4] = {0, 0, 0, 0};
    float totalVolume[4] = {0, 0, 0, 0};
    unsigned int totalTrans[4] = {0, 0, 0, 0};
    sleep(60);
    int j;
    // Array size

    for (; (tail != tradingInfoIndex); tail = (tail + 1) % BUFFER_SIZE)
    {

      data = tradingInfos[tail];
      for (j = 0; j < 4; j++)
      {
        if (strcmp(data.symbol, symbols[j]) == 0)
        {
          finalTradingInfos[j] = data;
          if (candleSticks[candleSticksIndex + j * 15].initialPrice == 0)
          {
            candleSticks[candleSticksIndex + j * 15].initialPrice = data.price;
          }
          if (data.price > candleSticks[candleSticksIndex + j * 15].maxPrice)
          {
            candleSticks[candleSticksIndex + j * 15].maxPrice = data.price;
          }
          if (data.price < candleSticks[candleSticksIndex + j * 15].minPrice)
          {
            candleSticks[candleSticksIndex + j * 15].minPrice = data.price;
          }
          candleSticks[candleSticksIndex + j * 15].totalVolume += data.volume;
          candleSticks[candleSticksIndex + j * 15].transactions++;
          candleSticks[candleSticksIndex + j * 15].volumePrice += data.volume * data.price;
        }
      }
    }

    for (j = 0; j < 4; j++)
    {
      candleSticks[candleSticksIndex + j * 15].finalPrice = finalTradingInfos[j].price;
      candleSticks[candleSticksIndex + j * 15].volumePrice = candleSticks[candleSticksIndex + j * 15].volumePrice / candleSticks[candleSticksIndex + j * 15].transactions;
    }

    for (j = 0; j < 4; j++)
    {
      char fileTrailing[30] = "_candleSticks_info.txt";
      char fileName[90] = "./";
      strcat(fileName, symbols[j]);
      strcat(fileName, "/");
      strcat(fileName, symbols[j]);
      strcat(fileName, fileTrailing);
      FILE *fptr = fopen(fileName, "a");
      fprintf(fptr, "Initial Price: %f \t Final Price: %f \t Max Price: %f \t Min Price: %f \t Total Volume: %f\n", candleSticks[candleSticksIndex + j * 15].initialPrice, candleSticks[candleSticksIndex + j * 15].finalPrice,
              candleSticks[candleSticksIndex + j * 15].maxPrice, candleSticks[candleSticksIndex + j * 15].minPrice, candleSticks[candleSticksIndex + j * 15].totalVolume);
      fclose(fptr);
    }

    candleSticksIndex++;

    if (candleSticksIndex == 15)
    {
      candleStickArrayFull = 1;
    }

    candleSticksIndex = candleSticksIndex % 15;



    if (candleStickArrayFull)
    {
      for (int i = 0; i < 4; i++)
      {
        for (j = 0; j < 15; j++)
        {
          totalVolume[i] += candleSticks[j + i * 15].totalVolume;
          mean[i] += candleSticks[j + i * 15].volumePrice;
          totalTrans[i] += candleSticks[j + i * 15].transactions;
        }
        if (totalTrans[i] > 0)
          mean[i] = mean[i] / totalTrans[i];
        else
          mean[i] = 0;
      }
    }

    else
    {
      for (int i = 0; i < 4; i++)
      {
        for (j = 0; j < candleSticksIndex; j++)
        {
          totalVolume[i] += candleSticks[j + i * 15].totalVolume;
          mean[i] += candleSticks[j + i * 15].volumePrice;
          totalTrans[i] += candleSticks[j + i * 15].transactions;
        }
        if (totalTrans[i] > 0)
          mean[i] = mean[i] / totalTrans[i];
        else
          mean[i] = 0;
      }
    }

    for (j = 0; j < 4; j++)
    {
      char fileTrailing[30] = "_15minutesAverage.txt";
      char fileName[90] = "./";
      strcat(fileName, symbols[j]);
      strcat(fileName, "/");
      strcat(fileName, symbols[j]);
      strcat(fileName, fileTrailing);
      FILE *fptr = fopen(fileName, "a");
      fprintf(fptr, "Average trade price/cost : %f, Total volume %f\n", mean[j], totalVolume[j]);
      fclose(fptr);
    }
  }
}

int main(void)
{

  struct sigaction act;
  act.sa_handler = INT_HANDLER;
  act.sa_flags = 0;
  sigemptyset(&act.sa_mask);
  sigaction(SIGINT, &act, 0);

  struct stat sb;
  int i = 0;
  for (i = 0; i < 4; i++)
  {
    if (!(stat(symbols[i], &sb) == 0 && S_ISDIR(sb.st_mode)))
    {
      mkdir(symbols[i], 0755);
    }
  }

  fifo = queueInit();

  struct lws_context *context = NULL;
  struct lws_context_creation_info info;
  struct lws *wsi = NULL;
  struct lws_protocols protocol;
  memset(&info, 0, sizeof (info));
  
  while(1){
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    char inputURL[300] =
          "ws.finnhub.io/?token=ccn1a2qad3i1nkreoa00ccn1a2qad3i1nkreoa0g";
    const char *urlProtocol, *urlTempPath;
    char urlPath[300];

    context = lws_create_context(&info);
    printf(KRED "[Main] context created.\n" RESET);

    while (context == NULL)
    {
      printf(KRED "[Main] context is NULL.\n" RESET);
      context = lws_create_context(&info);
    }
    struct lws_client_connect_info clientConnectionInfo;
    memset(&clientConnectionInfo, 0, sizeof(clientConnectionInfo));
    clientConnectionInfo.context = context;
    if (lws_parse_uri(inputURL, &urlProtocol, &clientConnectionInfo.address,
                       &clientConnectionInfo.port, &urlTempPath))
    {
      printf("Couldn't parse URL\n");
    }
    urlPath[0] = '/';
    strncpy(urlPath + 1, urlTempPath, sizeof(urlPath) - 2);
    urlPath[sizeof(urlPath) - 1] = '\0';
    clientConnectionInfo.port = 443;
    clientConnectionInfo.path = urlPath;
    clientConnectionInfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;

    clientConnectionInfo.host = clientConnectionInfo.address;
    clientConnectionInfo.origin = clientConnectionInfo.address;
    clientConnectionInfo.ietf_version_or_minus_one = -1;
    clientConnectionInfo.protocol = protocols[0].name;
    printf("Testing %s\n\n", clientConnectionInfo.address);
    printf("Connecticting to %s://%s:%d%s \n\n", urlProtocol,
            clientConnectionInfo.address, clientConnectionInfo.port, urlPath);
    wsi = lws_client_connect_via_info(&clientConnectionInfo);
    while (wsi == NULL)
    {
      printf(KRED "[Main] wsi create error.\n" RESET);
      printf("Try to connect..\n");
      wsi = lws_client_connect_via_info(&clientConnectionInfo);
    }
    printf(KGRN "[Main] wsi create success.\n" RESET);
    while(!destroy_flag)
    {
      lws_service(context, 0);
    }
    lws_context_destroy(context);
    
    destroy_flag = 0;
  }

  queueDelete(fifo);
  return 0;
}

queue *queueInit(void)
{
  queue *q;

  q = (queue *)malloc(sizeof(queue));
  if (q == NULL)
    return (NULL);

  q->empty = 1;
  q->full = 0;
  q->head = 0;
  q->tail = 0;
  q->mut = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
  pthread_mutex_init(q->mut, NULL);
  q->notFull = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
  pthread_cond_init(q->notFull, NULL);
  q->notEmpty = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
  pthread_cond_init(q->notEmpty, NULL);

  return (q);
}

void queueDelete(queue *q)
{
  pthread_mutex_destroy(q->mut);
  free(q->mut);
  pthread_cond_destroy(q->notFull);
  free(q->notFull);
  pthread_cond_destroy(q->notEmpty);
  free(q->notEmpty);
  free(q);
}

void queueAdd(queue *q, workFunc in)
{
  q->buf[q->tail] = in;

  q->tail++;
  if (q->tail == QUEUESIZE)
    q->tail = 0;
  if (q->tail == q->head)
    q->full = 1;
  q->empty = 0;

  return;
}

void queueDel(queue *q, workFunc *out)
{
  *out = q->buf[q->head];
  q->head++;
  if (q->head == QUEUESIZE)
    q->head = 0;
  if (q->head == q->tail)
    q->empty = 1;
  q->full = 0;

  return;
}

void *consumer()
{

  workFunc workpoint;

  // fifo = (queue *)q;

  while (1)
  {
    pthread_mutex_lock(fifo->mut);
    while (fifo->empty)
    {
      // printf ("consumer: queue EMPTY.\n");
      pthread_cond_wait(fifo->notEmpty, fifo->mut);
    }

    queueDel(fifo, &workpoint);

    workpoint.work(workpoint.arg); // execute the work function

    pthread_mutex_unlock(fifo->mut);
    pthread_cond_signal(fifo->notFull);
  }

  return (NULL);
}

void printInfoToFile(int index)
{
  struct timeval timeWrited;
  

  struct tradingInfo data = tradingInfos[index];

  char fileTrailing[20] = "_trading_info.txt";
  char fileTrailing2[20] = "_times.txt";

  char fileName[90] = "./";
  char fileName2[90] = "./";

  strcat(fileName, data.symbol);
  strcat(fileName, "/");
  strcat(fileName, data.symbol);
  strcat(fileName, fileTrailing);
  FILE *fptr = fopen(fileName, "a");
  fprintf(fptr, "Value: %f \t Symbol: %s \t Timestrap: %lu \t Volume: %f \n", data.price, data.symbol, data.timestrap, data.volume);
  gettimeofday(&timeWrited, NULL);
  fclose(fptr);

  strcat(fileName2, data.symbol);
  strcat(fileName2, "/");
  strcat(fileName2, data.symbol);
  strcat(fileName2, fileTrailing2);

  FILE *fptr2 = fopen(fileName2, "a");
  // Time Received Time writetd
  fprintf(fptr2, "%lu\t%u\n", data.timestrap, timeWrited);

  fclose(fptr2);
}
