
#include "stdafx.h"
#include "GSMPanicPrivate.h"
#include "SerialPortClient.h"
#include "UdpClient.h"

#include "GSMPanicLogging.h"
#include "GSMPanicConfig.h"

IGSMPanicProxy* GSMPanicControllerPrivate::instance = 0;

//--------------------------------------------------------------------------------------
//	Внутренняя реализация главного контроллера
//--------------------------------------------------------------------------------------
GSMPanicControllerPrivate::GSMPanicControllerPrivate()
	: 
	running(false),
	comPort(DEFAULT_COM_PORT),
	udpLocalPort(DEFAULT_UDP_LOCAL_PORT),
	udpTargetPort(DEFAULT_UDP_TARGET_PORT),
	balanceValid(false),
	simBalance(0.0),
	asio_serial_errors_counter(0),
	asio_udp_errors_counter(0),
	modemState(t_GSMState()),

	serial_io_service(), //отдельный io-service на каждый IO поток
	serial_io_work(serial_io_service), // создаем work object что-бы блокировать выход из ioloop
	udp_io_service(),
	udp_io_work(udp_io_service) // создаем work object что-бы блокировать выход из ioloop
{
	instance = this;

#ifdef LIB_MODE_CLIENT
	cb_PanicCallback = NULL;
	cb_ErrorCallback = NULL;
#endif

	LoggingOptions options;
	options.enableFileLogging = 
#if (defined(_DEBUG) || defined(LIB_MODE_CLIENT))
		true;
#else
		false;
#endif
	options.enableWinEventLogging = true;
	SetupLogging(options);

	G_LOG(ls_debug, "GSMPanicControllerPrivate инициализирован [конструктор]");
}

GSMPanicControllerPrivate::~GSMPanicControllerPrivate()
{
	stop();
	G_LOG(ls_debug, "GSMPanicControllerPrivate удален [деструктор]");
}

/*
	Запуск IO-loop контроллера
	async - если true, запускает поток асинхронно, иначе блокирует до завершения
*/
void GSMPanicControllerPrivate::start(bool async)
{
	if (running)
		return;

	//spawn io_thread with current io_service object
	io_thread = boost::thread(boost::bind(&GSMPanicControllerPrivate::run, this));
	if (!async)
		io_thread.join();
}

/*
	Остановка IO loop. 
*/
void GSMPanicControllerPrivate::stop()

	if (running){
		running = false;
		serial_io_service.stop();
		udp_io_service.stop();
		io_thread.join();
		G_LOG(ls_info, "Служба приема тревог по GSM остановлена");
	}
	
}

bool GSMPanicControllerPrivate::isRunning()
{
	return running;
}

/*
	Сброс и загрузка настроек
*/
bool GSMPanicControllerPrivate::init()
{
	resetGsmModemStates();
	return GSMPanicConfig::loadConfigFromRegistry();
}

/*
	Точка входа главного потока
*/
void GSMPanicControllerPrivate::run()
{
	running = true;
	G_LOG(ls_info, "Старт службы приема тревог по GSM");
	
	bool first_run = true;
	while (running){
		// загрузка настроек
		if (!init())
		{
			// ошибка инициализации - однократный вывод сообщения
			if (first_run) 
			{
				G_LOG(ls_warning, GSMPanicConfig::getEnabled() ? "Ошибка загрузки настроек из реестра Windows!" : "Настройки не сохранены или прием тревог ЗАПРЕЩЕН в конфигураторе!");
			}
		}
		else
		{
			G_LOG(ls_debug, "Запуск потоков ввода-вывода [GSMPanicControllerPrivate::run()]");
			try
			{
				// Инициализация IO thread
				boost::thread udp_thread = boost::thread(boost::bind(&GSMPanicControllerPrivate::udp_io_thread, this));
				boost::thread serial_thread = boost::thread(boost::bind(&GSMPanicControllerPrivate::serial_io_thread, this));

				udp_thread.join();
				serial_thread.join();
			}
			catch (boost::thread_interrupted &)
			{			
				G_LOG(ls_error, std::string("ERROR: Непредвиденное прерывание потока ввода-вывода"));
			}
		}

		// Попытка рестарта сервиса через таймаут
		if (running)
		{
			for (int i = 0; (i < CTRL_AUTO_RESTART_DELAY_S) && running; i++) 
			{
				boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
			}
		}

		first_run = false;
	}
}

/*
	Точка входа потока UDP
*/
void GSMPanicControllerPrivate::udp_io_thread()
{
	udp_io_service.reset();

	while (running){
		// загрузка настроек
		if (init())
		{
			try 
			{
				G_LOG(ls_debug, "Запуск потока обработки UDP [udp_io_thread()]");
				// форсируем очистку предыдущего клиента				
				udp_client.reset();
				// создание экземпляра клиента UDP
				udp_client.reset(new UdpClient(udp_io_service));			
				// запуск IO loop
				udp_io_service.run();
			}
			catch (boost::system::system_error& e)
			{
				// ошибки asio
				G_LOG( (asio_udp_errors_counter++) % ASIO_ERRORS_LOG_DIV == 0 ? ls_error : ls_debug, 
					std::string("Ошибка сетевой подсистемы в потоке UDP [udp_io_thread()]: ") + std::string(e.what()));
			}
			catch (std::exception& e)
			{
				// общие ошибки
				G_LOG(ls_error, std::string("Ошибка времени выполнения в потоке UDP [udp_io_thread()]: ") + std::string(e.what()));
			}

			udp_client.reset(); // обычное завершение потока
		}

		// Попытка рестарта сервиса через таймаут
		if (running) 
		{
			for (int i = 0; (i < CTRL_AUTO_RESTART_DELAY_S) && running; i++) 
			{
				boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
			}
		}
	}
}

/*
	Точка входа потока последовательного порта
*/
void GSMPanicControllerPrivate::serial_io_thread()
{	
	serial_io_service.reset();

	while (running){
		// загрузка настроек
		if (init())
		{
			try 
			{
				G_LOG(ls_debug, "Запуск потока обработки последовательного порта [serial_io_thread()]");
				// форсируем очистку предыдущего клиента		
				serial_client.reset();
				// создание экземпляра клиента последовательного порта
				serial_client.reset(new SerialPortClient(serial_io_service));				
				//l запуск IO loop
				serial_io_service.run();
			}
			catch (boost::system::system_error& e)
			{
				// ошибки asio
				G_LOG((asio_serial_errors_counter++) % ASIO_ERRORS_LOG_DIV == 0 ? ls_error : ls_debug, 
					std::string("Ошибка ввода-вывода в потоке последовательного порта [serial_io_thread()]: ") + std::string(e.what()));
				
				// проверка "переподключения" USB устройства
#ifdef ENABLE_USB_PORT_AUTO_RESCAN
				if (asio_serial_errors_counter >= MAX_IO_ERRORS_BEFORE_USB_RESCAN) 
				{
					asio_serial_errors_counter = 0;
					tryToCycleUsbDevice();
				}
#endif
			}
			catch (std::exception& e)
			{
				// общие ошибки
				G_LOG(ls_error, std::string("Ошибка времени выполнения в потоке последовательного порта [serial_io_thread()]: ") + std::string(e.what()));
			}

			serial_client.reset(); // обычный выход из потока
		}

		// Попытка рестарта сервиса через таймаут
		if (running) 
		{
			for (int i = 0; (i < CTRL_AUTO_RESTART_DELAY_S) && running; i++)
			{
				boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
			}
		}
	}
}

/*
	Переключение USB устройства
*/
void GSMPanicControllerPrivate::tryToCycleUsbDevice(){
#if (WINVER >= 0x0600) //only Windows Vista+
	// проверка опции в конфиге
	if (!GSMPanicConfig::getUsbPortAutoRescan())
		return;

	// проверить что USB устройство имеет виртуальный COM порт
	std::string hwid = GSMPanicConfig::getComPortHWID();
	if (hwid.empty() || !boost::starts_with(hwid, std::string("USB\\")))
		return;

	serial_client.reset(); // явно останавливаем клиент перед переключением
	G_LOG(ls_info, "Попытка переподключения USB порта...");

	startExternalProcess("devcon.exe remove =ports \"" + hwid + "\"", true);
	startExternalProcess("devcon.exe rescan", true);
#endif
}

/*
	Запуск внешнего процесса
*/
void GSMPanicControllerPrivate::startExternalProcess(std::string cmd, bool wait){
	char cmd_buf[1024] = {0};
	memcpy(cmd_buf, cmd.c_str(), cmd.size());

	STARTUPINFO si;
	PROCESS_INFORMATION pi;
    ZeroMemory( &si, sizeof(si) );
    si.cb = sizeof(si);
    ZeroMemory( &pi, sizeof(pi) );
	::CreateProcess(NULL, cmd_buf, NULL, NULL, FALSE, NULL, NULL, NULL, &si, &pi);
	
	if (wait) {
		int waitCnt = 0;	
		while (running && WAIT_TIMEOUT == ::WaitForSingleObject(pi.hProcess, 100) && waitCnt++ < 100);
	}
}

/*
	Переопределенные функции Прокси-интерфейса
*/
void GSMPanicControllerPrivate::panicCallNotification(std::string number)
{
	G_LOG(ls_warning, std::string("Зарегистрирован тревожный вызов с номера ") + number);
	if (udp_client.get()) 
	{
		udp_client->reportPanicCall(number);
	}
}

void GSMPanicControllerPrivate::modemStateChanged(int state)
{
	modemState.modemState = state;
}

void GSMPanicControllerPrivate::gsmStateChanged(int state)
{
	if (state != modemState.gsmState) 
	{
		if (state == GSM_NET_STATE_NOSIM)
			G_LOG(ls_error, "SIM карта отсутствует или заблокирована");
		else if (state == GSM_NET_STATE_ONLINE)
			G_LOG(ls_info, "Есть регистрация в сети GSM, уровень сигнала: "+ boost::lexical_cast<std::string>(getSignalLevel()));
		else if (state == GSM_NET_STATE_REG_DENIED)
			G_LOG(ls_error, "Отказ регистрации в сети GSM. Возмножно SIM карта заблокирована оператором.");
	}

	modemState.gsmState = state;
}

void GSMPanicControllerPrivate::gsmSignalLevelChanged(int rssi, int ber)
{
	modemState.bitErrorRate = ber;
	modemState.signalLevel = rssi;
}

void GSMPanicControllerPrivate::gsmErrorHandler(int err)
{
	G_LOG(ls_debug, std::string("CME Ошибка GSM терминала, код " + boost::lexical_cast<std::string>(err)));
}

int GSMPanicControllerPrivate::getGsmState()
{
	return modemState.gsmState;
}

int GSMPanicControllerPrivate::getSignalLevel()
{
	return modemState.signalLevel;
}

int GSMPanicControllerPrivate::getBitErrorRate(){
	return modemState.bitErrorRate;
}

int GSMPanicControllerPrivate::getModemState()
{
	return modemState.modemState;
}

void GSMPanicControllerPrivate::resetGsmModemStates(){
	resetGsmNetworkStates();
	modemState.modemState = GSM_MODEM_STATE_OFFLINE;
}
void GSMPanicControllerPrivate::resetGsmNetworkStates(){
	modemState.bitErrorRate = 0;
	modemState.gsmState = GSM_NET_STATE_INIT_PENDING;
	modemState.signalLevel = 0;	
}

IGSMPanicProxy* GSMPanicControllerPrivate::getProxyInstance(){
	return instance;
}

void GSMPanicControllerPrivate::invalidateBalance(){
	simBalance = 0;
	balanceValid = false;
}
void GSMPanicControllerPrivate::setBalance(double new_balance){
	simBalance = new_balance;
	balanceValid = true;
}

double GSMPanicControllerPrivate::getBalance(){
	return simBalance;
}

bool GSMPanicControllerPrivate::isBalanceValid(){
	return balanceValid;
}
//---------------------------------------------------------------------------------------------------------------


#ifdef LIB_MODE_CLIENT
void GSMPanicControllerPrivate::setPanicCallback(GSMPanicCallback cb){
	cb_PanicCallback = cb;
}
void GSMPanicControllerPrivate::setErrorCallback(GSMErrorCallback cb){
	cb_ErrorCallback = cb;
}
#endif
