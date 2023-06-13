import React from 'react';
import {
    Button,
    Modal, ModalVariant,
    TextArea,
    Pagination, PaginationVariant,
} from '@patternfly/react-core';
import {
    expandable,
    Table,
    TableHeader,
    TableBody,
    TableVariant,
    sortable,
    SortByDirection,
} from '@patternfly/react-table';
import {
    retrieveAllAcis, modifyLdapEntry
} from '../lib/utils.jsx';
import {
    getAciActualName, isAciPermissionAllow
} from '../lib/aciParser.jsx';
import AddNewAci from './operations/aciNew.jsx';
import { DoubleConfirmModal } from "../../notifications.jsx";

class AciWizard extends React.Component {
    constructor (props) {
        super(props);

        this.state = {
            firstLoad: true,
            page: 1,
            perPage: 10,
            value: '',
            sortBy: {},
            rows: [],
            columns: [
                { title: 'ACI Name', transforms: [sortable], cellFormatters: [expandable] },
                { title: 'ACI Rule Type', transforms: [sortable] },
            ],
            actions: [
                {
                    title: 'Edit ACI',
                    onClick: (event, rowId, rowData, extra) => this.showEditAci(rowData)
                },
                {
                    isSeparator: true
                },
                {
                    title: 'Remove ACI',
                    onClick: (event, rowId, rowData, extra) => this.showDeleteConfirm(rowData)
                },
            ],
            isWizardOpen: false,
            isManualOpen: false,
            showModal: true,
            showConfirmDelete: false,
            modalSpinning: false,
            modalChecked: false,
            showEditAci: false,
            attrName: "",
            aciText: "",
            aciTextNew: "",
        };

        this.onToggle = isOpen => {
            this.setState({
                isOpen
            });
        };

        this.handleSetPage = (_event, pageNumber) => {
            this.setState({
                page: pageNumber
            });
        };

        this.handlePerPageSelect = (_event, perPage) => {
            this.setState({
                perPage,
                page: 1
            });
        };

        this.handleCloseModal = () => {
            this.setState({
                showModal: false,
            });
        };

        this.onChange = (e) => {
            // Handle the modal changes
            const value = e.target.type === 'checkbox' ? e.target.checked : e.target.value;
            this.setState({
                [e.target.id]: value,
            });
        };

        this.showDeleteConfirm = (rowData) => {
            this.setState({
                aciText: rowData.fullAci,
                aciName: rowData.cells[0],
                showDeleteConfirm: true,
                modalChecked: false,
                modalSpinning: false,
            });
        };

        this.closeDeleteConfirm = (rowData) => {
            this.setState({
                aciText: "",
                showDeleteConfirm: false,
                modalChecked: false,
                modalSpinning: false,
            });
        };

        this.showEditAci = (rowData) => {
            this.setState({
                aciText: rowData.fullAci,
                aciTextNew: rowData.fullAci,
                showEditAci: true,
                modalChecked: false,
                modalSpinning: false,
            });
        };

        this.handleCloseEditAci = () => {
            this.setState({
                showEditAci: false,
            });
        };

        this.handleResetACIText = () => {
            const orig = this.state.aciText;
            this.setState({
                aciTextNew: orig
            });
        };

        this.handleSaveACI = () => {
            const params = { serverId: this.props.editorLdapServer };
            const ldifArray = [];
            ldifArray.push(`dn: ${this.props.wizardEntryDn}`);
            ldifArray.push('changetype: modify');
            ldifArray.push('delete: aci');
            ldifArray.push(`aci: ${this.state.aciText}`);
            ldifArray.push('-');
            ldifArray.push(`add: aci`);
            ldifArray.push(`aci: ${this.state.aciTextNew}`);

            this.setState({
                modalSpinning: true
            });

            modifyLdapEntry(params, ldifArray, (result) => {
                if (result.errorCode === 0) {
                    this.props.addNotification(
                        "success",
                        `Successfully replace ACI`
                    );
                    this.buildAciTable();
                } else {
                    this.props.addNotification(
                        "error",
                        `Failed to update ACI: error ${result.errorCode} - ${result.output}`
                    );
                }
                this.handleCloseEditAci();
                const opInfo = { // This is what refreshes treeView
                    operationType: 'MODIFY',
                    resultCode: result.errorCode,
                    time: Date.now()
                };
                this.props.setWizardOperationInfo(opInfo);
            });
        };

        this.deleteACI = () => {
            const params = { serverId: this.props.editorLdapServer };
            const ldifArray = [];
            ldifArray.push(`dn: ${this.props.wizardEntryDn}`);
            ldifArray.push('changetype: modify');
            ldifArray.push('delete: aci');
            ldifArray.push(`aci: ${this.state.aciText}`);

            this.setState({
                modalSpinning: true
            });

            modifyLdapEntry(params, ldifArray, (result) => {
                if (result.errorCode === 0) {
                    this.props.addNotification(
                        "success",
                        `Successfully removed ACI`
                    );
                    this.buildAciTable();
                } else {
                    this.props.addNotification(
                        "error",
                        `Failed to remove ACI: error ${result.errorCode} - ${result.output}`
                    );
                }
                this.closeDeleteConfirm();
                const opInfo = { // This is what refreshes treeView
                    operationType: 'MODIFY',
                    resultCode: result.errorCode,
                    time: Date.now()
                };
                this.props.setWizardOperationInfo(opInfo);
            });
        };

        this.handleAddAciManual = () => {
            const params = { serverId: this.props.editorLdapServer };
            const ldifArray = [];
            ldifArray.push(`dn: ${this.props.wizardEntryDn}`);
            ldifArray.push('changetype: modify');
            ldifArray.push(`add: aci`);
            ldifArray.push(`aci: ${this.state.aciTextNew}`);

            this.setState({
                modalSpinning: true
            });

            modifyLdapEntry(params, ldifArray, (result) => {
                if (result.errorCode === 0) {
                    this.props.addNotification(
                        "success",
                        `Successfully added ACI`
                    );
                    this.buildAciTable();
                } else {
                    this.props.addNotification(
                        "error",
                        `Failed to add ACI: error ${result.errorCode} - ${result.output}`
                    );
                }
                this.handleToggleManual();
                const opInfo = { // This is what refreshes the treeView
                    operationType: 'MODIFY',
                    resultCode: result.errorCode,
                    time: Date.now()
                };
                this.props.setWizardOperationInfo(opInfo);
            });
        };

        // this.buildAciTable = this.buildAciTable.bind(this);
        this.handleSort = this.handleSort.bind(this);
        this.handleCollpase = this.handleCollpase.bind(this);
    } // End constructor

    componentDidMount () {
        this.buildAciTable(this.state.firstLoad);
        this.setState({
            firstLoad: false
        });
    }

    buildAciTable = (firstLoad) => {
        const params = {
            serverId: this.props.editorLdapServer,
            baseDn: this.props.wizardEntryDn
        };
        retrieveAllAcis(params, (resultArray) => {
            const rows = [];
            const columns = [...this.state.columns];
            const actions = this.state.actions;
            let count = 0;

            if (resultArray.length !== 0) {
                const myAciArray = [...resultArray[0].aciArray];
                for (const anAci of myAciArray) {
                    const aciName = getAciActualName(anAci);
                    const aciAllow = isAciPermissionAllow(anAci);

                    rows.push({
                        isOpen: false,
                        cells: [aciName, aciAllow ? "allow" : "deny"],
                        fullAci: anAci
                    });
                    rows.push({
                        parent: count,
                        fullWidth: true,
                        cells: [anAci]
                    });
                    count += 2;
                }
            }

            this.setState({
                rows,
                columns,
                actions,
            }, () => { if (!firstLoad) { this.props.onReload() } }); // refreshes table view
        });
    };

    handleCollpase(event, rowKey, isOpen) {
        const { rows, perPage, page } = this.state;
        const index = (perPage * (page - 1) * 2) + rowKey; // Adjust for page set
        rows[index].isOpen = isOpen;
        this.setState({
            rows
        });
    }

    handleSort(_event, index, direction) {
        const sorted_rows = [];
        const rows = [];
        let count = 0;

        // Convert the rows into a sortable array
        for (const aci of this.state.rows) {
            if (aci.cells[1]) {
                sorted_rows.push({
                    name: aci.cells[0],
                    type: aci.cells[1],
                    fullAci: aci.fullAci,
                });
            }
        }

        // Sort the old rows and build new rows
        if (index === 1) {
            sorted_rows.sort((a, b) => (a.name > b.name) ? 1 : -1);
        } else {
            sorted_rows.sort((a, b) => (a.type > b.type) ? 1 : -1);
        }
        if (direction !== SortByDirection.asc) {
            sorted_rows.reverse();
        }
        for (const aci of sorted_rows) {
            rows.push({
                isOpen: false,
                cells: [aci.name, aci.type],
                fullAci: aci.fullAci,
            });
            rows.push({
                parent: count,
                fullWidth: true,
                cells: [aci.fullAci]
            });
            count += 2;
        }

        this.setState({
            sortBy: {
                index,
                direction
            },
            rows,
            page: 1,
        });
    }

    handleToggleWizard = () => {
        this.setState({
            isWizardOpen: !this.state.isWizardOpen
        });
    };

    onToggleWizard = () => {
        this.setState({
            isWizardOpen: !this.state.isWizardOpen
        });
    };

    handleToggleManual = () => {
        this.setState({
            isManualOpen: !this.state.isManualOpen,
            aciTextNew: "",
            modalSpinning: false,
        });
    };

    render () {
        // We are using an expandable list, so every row has a child row with an
        // index that points back to the parent.  So when we splice the rows for
        // pagination we have to treat each connection as two rows, and we need
        // to rewrite the child's parent index to point to the correct location
        // in the new spliced array
        const { columns, rows, perPage, page, sortBy, showModal, actions, modalSpinning } = this.state;
        const origRows = [...rows];
        const startIdx = ((perPage * page) - perPage) * 2;
        let tableRows = origRows.splice(startIdx, perPage * 2);
        for (let idx = 1, count = 0; idx < tableRows.length; idx += 2, count += 2) {
            // Rewrite parent index to match new spliced array
            tableRows[idx].parent = count;
        }

        // Edit modal
        let btnName = "Save ACI";
        const extraPrimaryProps = {};
        if (modalSpinning) {
            btnName = "Saving ACI ...";
            extraPrimaryProps.spinnerAriaValueText = "Loading";
        }

        const title = "Manage ACI's For " + this.props.wizardEntryDn;

        // Handle table state
        let cols = columns;
        if (rows.length === 0) {
            tableRows = [{ cells: ["No ACI's"] }];
            cols = [{ title: 'Access Control Instructions' }];
        }

        return (
            <>
                <Modal
                    variant={ModalVariant.medium}
                    className="ds-modal-select"
                    title={title}
                    isOpen={showModal}
                    onClose={this.handleCloseModal}
                    actions={[
                        <Button
                            key="acc aci"
                            variant="primary"
                            onClick={this.handleToggleWizard}
                        >
                            Add ACI Wizard
                        </Button>,
                        <Button
                            key="acc aci manual"
                            variant="primary"
                            onClick={this.handleToggleManual}
                        >
                            Add ACI Manually
                        </Button>,
                        <Button key="cancel" variant="link" onClick={this.handleCloseModal}>
                            Close
                        </Button>
                    ]}
                >
                    <Table
                        className="ds=margin-top-lg"
                        aria-label="Expandable table"
                        cells={cols}
                        rows={tableRows}
                        actions={rows.length > 0 ? actions : null}
                        onCollapse={this.handleCollpase}
                        variant={TableVariant.compact}
                        sortBy={sortBy}
                        onSort={this.handleSort}
                        key={tableRows}
                    >
                        <TableHeader />
                        <TableBody />
                    </Table>
                    {rows.length > 0 &&
                        <Pagination
                            itemCount={rows.length / 2}
                            widgetId="pagination-options-menu-bottom"
                            perPage={perPage}
                            page={page}
                            variant={PaginationVariant.bottom}
                            onSetPage={this.handleSetPage}
                            onPerPageSelect={this.handlePerPageSelect}
                        />}
                    <div className="ds-margin-top-xlg" />
                    {this.state.isWizardOpen &&
                        <AddNewAci
                            wizardEntryDn={this.props.wizardEntryDn}
                            editorLdapServer={this.props.editorLdapServer}
                            onReload={this.props.onReload}
                            refreshAciTable={this.buildAciTable}
                            isWizardOpen={this.state.isWizardOpen}
                            handleToggleWizard={this.onToggleWizard}
                            setWizardOperationInfo={this.props.setWizardOperationInfo}
                            treeViewRootSuffixes={this.props.treeViewRootSuffixes}
                            addNotification={this.props.addNotification}
                        />}
                    <DoubleConfirmModal
                        showModal={this.state.showDeleteConfirm}
                        closeHandler={this.closeDeleteConfirm}
                        handleChange={this.onChange}
                        actionHandler={this.deleteACI}
                        spinning={modalSpinning}
                        item={this.state.aciName}
                        checked={this.state.modalChecked}
                        mTitle="Delete ACI"
                        mMsg="Are you sure you want to delete this ACI?"
                        mSpinningMsg="Deleting ..."
                        mBtnName="Delete ACI"
                    />
                </Modal>
                <Modal
                    variant={ModalVariant.medium}
                    title="Edit ACI"
                    isOpen={this.state.showEditAci}
                    onClose={this.handleCloseEditAci}
                    actions={[
                        <Button
                            key="acc aci"
                            variant="primary"
                            onClick={this.handleSaveACI}
                            isLoading={modalSpinning}
                            spinnerAriaValueText={modalSpinning ? "Loading" : undefined}
                            {...extraPrimaryProps}
                            isDisabled={this.state.aciText === this.state.aciTextNew || modalSpinning}
                        >
                            {btnName}
                        </Button>,
                        <Button key="cancel" variant="link" onClick={this.handleCloseEditAci}>
                            Close
                        </Button>
                    ]}
                >
                    <TextArea
                        className="ds-textarea"
                        id="aciTextNew"
                        value={this.state.aciTextNew}
                        onChange={(str, e) => { this.onChange(e) }}
                        aria-label="aci text edit area"
                        autoResize
                        resizeOrientation="vertical"
                    />
                    <Button
                        className="ds-margin-top"
                        key="reset"
                        variant="secondary"
                        onClick={this.handleResetACIText}
                        isDisabled={this.state.aciText === this.state.aciTextNew}
                        isSmall
                    >
                        Reset ACI
                    </Button>
                </Modal>
                <Modal
                    variant={ModalVariant.medium}
                    title="Add ACI Manually"
                    isOpen={this.state.isManualOpen}
                    onClose={this.handleToggleManual}
                    actions={[
                        <Button
                            key="acc aci manual add"
                            variant="primary"
                            onClick={this.handleAddAciManual}
                            isLoading={modalSpinning}
                            spinnerAriaValueText={modalSpinning ? "Loading" : undefined}
                            {...extraPrimaryProps}
                            isDisabled={this.state.aciTextNew === "" || modalSpinning}
                        >
                            {btnName}
                        </Button>,
                        <Button key="cancel" variant="link" onClick={this.handleToggleManual}>
                            Close
                        </Button>
                    ]}
                >
                    <TextArea
                        className="ds-textarea"
                        id="aciTextNew"
                        value={this.state.aciTextNew}
                        onChange={(str, e) => { this.onChange(e) }}
                        aria-label="aci text edit area"
                        autoResize
                        resizeOrientation="vertical"
                        placeholder="Enter ACI ..."
                    />
                </Modal>
            </>
        );
    }
}

export default AciWizard;
