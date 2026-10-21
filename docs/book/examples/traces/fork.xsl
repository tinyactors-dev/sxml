<xsl:stylesheet version="4.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>
  <xsl:param name="archive" select="'archive.xml'"/>

  <!-- Stream the archive the parameter names, and make two downward
       passes over it at once. -->
  <xsl:template name="xsl:initial-template">
    <xsl:source-document streamable="yes" href="{$archive}">
      <xsl:fork>
        <xsl:sequence select="count(traces/trace)"/>
        <xsl:sequence select="count(traces/trace//span)"/>
      </xsl:fork>
    </xsl:source-document>
  </xsl:template>
</xsl:stylesheet>
