#include <qfiledialog.h>
#include <QtXml>
#include <qdebug.h>
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "GDSClientAPI_gUSBamp.h"
#include "GDSClientAPI_gHIamp.h"
#include "GDSClientAPI_gNautilus.h"


// GDS_DeviceInfo contains basic identifying information for each found device.
struct GDS_DeviceInfo
{
	uint32_t index;
	bool inUse;
	QString name;
	GDS_DEVICE_TYPE type;
};


bool MainWindow::handleResult(std::string calling_func, GDS_RESULT ret)
{
	bool res = true;
	if (ret.ErrorCode)
	{
		qDebug() << "ERROR on " << QString::fromStdString(calling_func) << ":" << ret.ErrorMessage;
		res = false;
	}
	return res;
}


void MainWindow::dataReadyCallback(GDS_HANDLE connectionHandle, void* usrData)
{
	MainWindow* thisWin = reinterpret_cast< MainWindow* >(usrData);
	float* dataBuffer = thisWin->m_dataBuffer[0].data();
	size_t bufferSize = thisWin->m_dataBuffer.size() * thisWin->m_dataBuffer[0].size();
	size_t scans_available = 0;  // Read as many scans as possible, up to as many will fit in m_dataBuffer
	GDS_RESULT res = GDS_GetData(thisWin->m_connectionHandle, &scans_available, dataBuffer, bufferSize);
	if (scans_available > 0)
	{
		//if (scans_available == thisWin->m_dataBuffer.size())
		//	thisWin->m_eegOutlet->push_chunk(thisWin->m_dataBuffer);
		//else if(scans_available > 0)
		thisWin->m_eegOutlet->push_chunk_multiplexed(dataBuffer, scans_available * thisWin->m_dataBuffer[0].size());
	}
}


MainWindow::MainWindow(QWidget *parent, const QString config_file)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    load_config(config_file);
	GDS_Initialize();  // Initialize the g.NEEDaccess library
}

MainWindow::~MainWindow()
{
	GDS_Uninitialize();
    delete ui;
}

void MainWindow::on_scanPushButton_clicked()
{
	bool success = true;

    ui->scanPushButton->setText("Scanning...");
    ui->scanPushButton->setDisabled(true);

	strcpy(this->m_hostEndpoint.IpAddress, ui->lineEdit_serverip->text().toLatin1().data());
	this->m_hostEndpoint.Port = ui->serverPortSpinBox->value();
	this->m_localEndpoint.Port = ui->clientPortSpinBox->value();
	this->m_updateRate = ui->updateRateSpinBox->value();

	GDS_DEVICE_CONNECTION_INFO* connected_devices = NULL;
	size_t count_daq_units = 0;
	handleResult("GDS_GetConnectedDevices",
		GDS_GetConnectedDevices(m_hostEndpoint, m_localEndpoint, &connected_devices, &count_daq_units));

	std::vector<GDS_DeviceInfo> new_devices;
	uint32_t count_devices = 0;
	for (size_t i = 0; i < count_daq_units; i++)
	{
		// Each daq unit operates a different number of devices. The second "for" loop 
		// addresses the devices attached to the daq unit.
		for (size_t j = 0; j < connected_devices[i].ConnectedDevicesLength; j++)
		{
			GDS_DeviceInfo new_info;
			new_info.index = count_devices;
			new_info.inUse = bool(connected_devices[i].InUse) == true;
			new_info.name = connected_devices[i].ConnectedDevices[j].Name;
			new_info.type = connected_devices[i].ConnectedDevices[j].DeviceType;
			new_devices.push_back(new_info);
			count_devices++;
		}
	}
	success &= handleResult("GDS_FreeConnectedDevicesList",
		GDS_FreeConnectedDevicesList(&connected_devices, count_daq_units));

	if (new_devices.size() > 0)
	{
		ui->availableListWidget->clear();
		QStringList newDeviceNames;
		for (auto it = new_devices.begin(); it != new_devices.end(); ++it) {
			ui->availableListWidget->addItem(it->name);
			// TODO: Use other GDS_DeviceInfo fields
		}
		ui->availableListWidget->setCurrentRow(0);
	}
	ui->scanPushButton->setText("Scan");
	ui->scanPushButton->setDisabled(false);
	//ui->connectPushButton->setDisabled(false);
}


void MainWindow::on_availableListWidget_itemSelectionChanged()
{
	ui->connectPushButton->setDisabled(false);
}


void MainWindow::on_connectPushButton_clicked()
{
	ui->goPushButton->setDisabled(true);
	QString pbtext = ui->connectPushButton->text();
	if (pbtext == "Connect")
	{
		QString deviceName = ui->availableListWidget->currentItem()->text();
		char(*device_names)[DEVICE_NAME_LENGTH_MAX] = new char[1][DEVICE_NAME_LENGTH_MAX];
		std::strcpy(device_names[0], deviceName.toLatin1().data());
		BOOL is_creator = FALSE;
		if (handleResult("GDS_Connect",
			GDS_Connect(m_hostEndpoint, m_localEndpoint, device_names, 1, TRUE, &m_connectionHandle, &is_creator)))
		{
			ui->connectPushButton->setText("Disconnect");
			ui->goPushButton->setDisabled(false);
		}
	}
	else
	{
		bool success = handleResult("GDS_Disconnect",
			GDS_Disconnect(&m_connectionHandle));
		if (success)
		{
			ui->connectPushButton->setText("Connect");
		}
		ui->goPushButton->setDisabled(true);
	}
	ui->connectPushButton->setDisabled(false);
}

void MainWindow::on_goPushButton_clicked()
{
	ui->goPushButton->setDisabled(true);  // re-enable when process complete.
	QString pbtext = ui->goPushButton->text();
	if (pbtext == "Stop!")
	{
		//m_thread.stopStreams();
		handleResult("GDS_SetDataReadyCallback",
			GDS_SetDataReadyCallback(m_connectionHandle, NULL, 0, this));
		this->m_eegOutlet = nullptr;
		handleResult("GDS_StopStreaming", GDS_StopStreaming(m_connectionHandle));
		handleResult("GDS_StopAcquisition", GDS_StopAcquisition(m_connectionHandle));
		ui->goPushButton->setText("Go!");
	}
	else  //pbtext == "Go!"
	{
		bool success = true;

		// TODO: GDS_SetDataAcquisitionErrorCallback
		// TODO: GDS_SetServerDiedCallback

		// Get the device configuration(s)
		GDS_CONFIGURATION_BASE *deviceConfigurations = NULL;
		size_t deviceConfigurationsCount = 0;
		// Note: API allocates memory during GDS_GetConfiguration. We must free it with GDS_FreeConfigurationList.
		success &= handleResult("GDS_GetConfiguration",
			GDS_GetConfiguration(m_connectionHandle, &deviceConfigurations, &deviceConfigurationsCount));

		// Fill in some of dev_info for the selected device.
		dev_info_type dev_info;
		int dev_ix = ui->availableListWidget->currentRow();
		dev_info.name = ui->availableListWidget->currentItem()->text().toStdString();
			
		// GDS_<DEVICE>_GetChannelNames requires a device_names argument.
		// The API wants an array of char[DEVICE_NAME_LENGTH_MAX] even though these functions are designed for only one device.
		char(*device_names)[DEVICE_NAME_LENGTH_MAX] = new char[1][DEVICE_NAME_LENGTH_MAX];
		std::strcpy(device_names[0], ui->availableListWidget->currentItem()->text().toLatin1().data());

		GDS_CONFIGURATION_BASE cfg = deviceConfigurations[dev_ix];
		if (cfg.DeviceInfo.DeviceType == GDS_DEVICE_TYPE_NOT_SUPPORTED)
		{
			success = false;
			qDebug() << "Unknown device type.";
		}
		else if (cfg.DeviceInfo.DeviceType == GDS_DEVICE_TYPE_GUSBAMP)
		{
			GDS_GUSBAMP_CONFIGURATION* cfg_dev = (GDS_GUSBAMP_CONFIGURATION*)cfg.Configuration;
			dev_info.nominal_srate = double(cfg_dev->SampleRate);
			qDebug() << "TODO: Get g.USBamp configuration. I do not have a device to test...";
		}
		else if (cfg.DeviceInfo.DeviceType == GDS_DEVICE_TYPE_GHIAMP)
		{
			GDS_GHIAMP_CONFIGURATION* cfg_dev = (GDS_GHIAMP_CONFIGURATION*)cfg.Configuration;
			dev_info.nominal_srate = double(cfg_dev->SamplingRate);
			qDebug() << "TODO: Get g.HIamp configuration. I do not have a device to test...";
		}
		else if (cfg.DeviceInfo.DeviceType == GDS_DEVICE_TYPE_GNAUTILUS)
		{
			GDS_GNAUTILUS_CONFIGURATION* cfg_dev = (GDS_GNAUTILUS_CONFIGURATION*)cfg.Configuration;
			dev_info.nominal_srate = double(cfg_dev->SamplingRate);
			dev_info.scans_per_block = size_t(cfg_dev->NumberOfScans);

			// Get channel names
			// First determine how many channel names there are.
			uint32_t mountedModulesCount = 0;
			size_t electrodeNamesCount = 0;
			success &= handleResult("GDS_GNAUTILUS_GetChannelNames",
				GDS_GNAUTILUS_GetChannelNames(m_connectionHandle, device_names, &mountedModulesCount, NULL, &electrodeNamesCount));
			// Allocate memory to store the channel names.
			char(*electrode_names)[GDS_GNAUTILUS_ELECTRODE_NAME_LENGTH_MAX] = new char[electrodeNamesCount][GDS_GNAUTILUS_ELECTRODE_NAME_LENGTH_MAX];
			success &= handleResult("GDS_GNAUTILUS_GetChannelNames",
				GDS_GNAUTILUS_GetChannelNames(m_connectionHandle, device_names, &mountedModulesCount, electrode_names, &electrodeNamesCount));
			// Copy the result into dev_info
			for (int chan_ix = 0; chan_ix < electrodeNamesCount; chan_ix++)
			{
				chan_info_type new_chan_info;
				new_chan_info.enabled = cfg_dev->Channels[chan_ix].Enabled;
				if (chan_ix < electrodeNamesCount)
				{
					new_chan_info.label = electrode_names[chan_ix];
					new_chan_info.type = "EEG";
					new_chan_info.unit = "uV";
				}
				else
				{
					// TODO: digital / other inputs. Maybe a separate stream?
					new_chan_info.type = "Unknown";
					new_chan_info.unit = "Unknown";
				}
				dev_info.channel_infos.push_back(new_chan_info);
			}
			delete[] electrode_names;
			electrode_names = NULL;
		}			
		delete[] device_names;
		device_names = NULL;

		success &= handleResult("GDS_SetDataReadyCallback",
			GDS_SetDataReadyCallback(m_connectionHandle, dataReadyCallback, dev_info.scans_per_block, this));

		// Free the resources used by the config
		success &= handleResult("GDS_FreeConfigurationList",
			GDS_FreeConfigurationList(&deviceConfigurations, deviceConfigurationsCount));

		// Get the number of devices and the number of samples per 1 scan (aka samples per frame)
		size_t n_scans = 1;
		size_t ndevices_with_channels = 0;
		success &= handleResult("GDS_GetDataInfo",
			GDS_GetDataInfo(m_connectionHandle, &n_scans, NULL, &ndevices_with_channels, &(dev_info.nsamples_per_scan)));

		// Now that we know ndevices_with_channels, get the number of channels in each device.
		size_t* channels_per_device = new size_t[ndevices_with_channels];
		success &= handleResult("GDS_GetDataInfo",
			GDS_GetDataInfo(m_connectionHandle, &n_scans, channels_per_device, &ndevices_with_channels, &(dev_info.nsamples_per_scan)));
		dev_info.channel_count = int(channels_per_device[dev_ix]);
		delete[] channels_per_device;
		channels_per_device = NULL;

		size_t buffer_size_in_samples = dev_info.scans_per_block * dev_info.nsamples_per_scan;  // Allocate a 1-sec buffer.
		m_dataBuffer.resize(dev_info.nominal_srate);
		for (int i = 0; i < dev_info.nominal_srate; ++i)
		{
			m_dataBuffer[i].resize(dev_info.nsamples_per_scan);
		}

		// Create outlet.
		lsl::stream_info gdsInfo(
			"gNEEDaccessData", "EEG",
			dev_info.channel_count, dev_info.nominal_srate,
			dev_info.channel_format, dev_info.name);
		// Append device meta-data
		gdsInfo.desc().append_child("acquisition")
			.append_child_value("manufacturer", "g.Tec")
			.append_child_value("model", "g.NEEDaccess client");
		// Append channel info
		lsl::xml_element channels_element = gdsInfo.desc().append_child("channels");
		for (auto it = dev_info.channel_infos.begin(); it < dev_info.channel_infos.end(); it++)
		{
			if (it->enabled)
			{
				channels_element.append_child("channel")
					.append_child_value("label", it->label)
					.append_child_value("type", it->type)
					.append_child_value("unit", it->unit);
			}
		}
		this->m_eegOutlet = new lsl::stream_outlet(gdsInfo);
		
		success &= handleResult("GDS_StartAcquisition", GDS_StartAcquisition(m_connectionHandle));
		success &= handleResult("GDS_StartStreaming", GDS_StartStreaming(m_connectionHandle));

		if (!success)
		{
			qDebug() << "Something failed while initializing acquisitions.";
		}

		ui->goPushButton->setText("Stop!");
	}
	ui->goPushButton->setDisabled(false);
}


void MainWindow::handleOutletsStarted(bool success)
{
	if (success)
	{
		ui->goPushButton->setText("Stop!");
	}
	else
	{
		handleResult("GDS_StopStreaming", GDS_StopStreaming(m_connectionHandle));
		handleResult("GDS_StopAcquisition", GDS_StopAcquisition(m_connectionHandle));
	}
	ui->goPushButton->setDisabled(false);
}


void MainWindow::handleDataSent(int nSamples)
{
	qDebug() << nSamples;
}


void MainWindow::load_config(const QString filename)
{
    QFile* xmlFile = new QFile(filename);
    if (!xmlFile->open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "Could not load XML from file " << filename;
        return;
    }
    QXmlStreamReader* xmlReader = new QXmlStreamReader(xmlFile);
    while (!xmlReader->atEnd() && !xmlReader->hasError()) {
        // Read next element
        xmlReader->readNext();
        if (xmlReader->isStartElement() && xmlReader->name() != "settings")
        {
            QStringRef elname = xmlReader->name();
            if (elname == "todo-setting-xml-element")
            {
                // TODO: Do something with the setting.
            }
        }
    }
    if (xmlReader->hasError()) {
        qDebug() << "Config file parse error "
            << xmlReader->error()
            << ": "
            << xmlReader->errorString();
    }
    xmlReader->clear();
    xmlFile->close();
}